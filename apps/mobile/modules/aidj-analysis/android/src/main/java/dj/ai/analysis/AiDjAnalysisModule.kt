package dj.ai.analysis

import android.content.Context
import android.net.Uri
import android.os.BatteryManager
import android.os.PowerManager
import expo.modules.kotlin.Promise
import expo.modules.kotlin.exception.CodedException
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean

class AnalysisException(code: String, message: String) :
  CodedException(code, message, null)

/**
 * Background track analysis.
 *
 * Analysis is expensive - a 50 minute mix is minutes of sustained CPU - so
 * this module is built around being interruptible rather than fast:
 *
 * - one track at a time, because parallel analyses only contend for the same
 *   CPU and thermal budget while making every individual result later
 * - checked against thermal status and battery level between tracks, and
 *   abandoned mid-track if the device gets hot
 * - resumable, because the caller records each finished track and never asks
 *   for it again
 *
 * The alternative - analysing everything as fast as possible - produces a hot
 * phone and a flat battery, which reads to the user as a broken app.
 */
class AiDjAnalysisModule : Module() {

  private var job: Job? = null
  private val cancelled = AtomicBoolean(false)

  private val context: Context
    get() = appContext.reactContext
      ?: throw AnalysisException("NO_CONTEXT", "No application context.")

  /**
   * True when the device is too hot to justify more analysis.
   *
   * THROTTLING already means the system is shedding performance; pushing on
   * would slow the foreground app and cook the battery for a task the user is
   * not waiting on.
   */
  private fun isThermallyThrottled(): Boolean {
    val power = context.getSystemService(Context.POWER_SERVICE) as? PowerManager
      ?: return false
    return power.currentThermalStatus >= PowerManager.THERMAL_STATUS_MODERATE
  }

  private fun batteryPercent(): Int {
    val battery = context.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
      ?: return 100
    return battery.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)
  }

  private fun isCharging(): Boolean {
    val battery = context.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
      ?: return false
    return battery.isCharging
  }

  /** Resolves a content:// URI to an fd:// URI the native decoder can open. */
  private fun resolveForNative(uri: String): String {
    if (!uri.startsWith("content://")) return uri

    val descriptor = try {
      context.contentResolver.openFileDescriptor(Uri.parse(uri), "r")
    } catch (error: Exception) {
      null
    } ?: throw AnalysisException("FILE_NOT_FOUND", "Cannot open $uri")

    val length = descriptor.statSize
    if (length <= 0) {
      descriptor.close()
      throw AnalysisException("DECODER_IO_ERROR", "Empty or unsized file.")
    }

    return "fd://${descriptor.detachFd()}?offset=0&length=$length"
  }

  override fun definition() = ModuleDefinition {
    Name("AiDjAnalysis")

    Events("onAnalysisProgress")

    /**
     * Whether conditions currently favour background analysis. The JS layer
     * checks this before queueing work so the decision is visible and
     * testable rather than buried in native code.
     */
    Function("canAnalyseNow") {
      val percent = batteryPercent()
      val charging = isCharging()
      val throttled = isThermallyThrottled()

      mapOf(
        "allowed" to (!throttled && (charging || percent > 20)),
        "batteryPercent" to percent,
        "isCharging" to charging,
        "thermallyThrottled" to throttled
      )
    }

    /**
     * Analyses one track. Rejects rather than returning a partial result if
     * cancelled - a half-analysed track has no valid tempo, and storing one
     * would poison the cache.
     */
    AsyncFunction("analyseTrack") { trackId: Int, uri: String, promise: Promise ->
      cancelled.set(false)

      job = CoroutineScope(Dispatchers.Default).launch {
        try {
          val nativeUri = resolveForNative(uri)

          val callback = object : NativeAnalyser.ProgressCallback {
            private var lastEmit = 0L

            override fun onProgress(decodedMs: Double, totalMs: Double): Boolean {
              if (cancelled.get()) return false
              // Abandon mid-track when the device heats up. Checked here
              // rather than only between tracks because a single long mix can
              // run for minutes.
              if (isThermallyThrottled()) return false

              // Throttle events to ~4/s: the JS thread does not need 90
              // updates a second, and each one costs a bridge crossing.
              val now = System.currentTimeMillis()
              if (now - lastEmit > 250) {
                lastEmit = now
                sendEvent(
                  "onAnalysisProgress",
                  mapOf(
                    "trackId" to trackId,
                    "decodedMs" to decodedMs,
                    "totalMs" to totalMs
                  )
                )
              }
              return true
            }
          }

          val flat = NativeAnalyser.nativeAnalyse(nativeUri, callback)
          if (flat == null) {
            promise.reject(
              AnalysisException(
                "ANALYSIS_FAILED",
                "The track could not be analysed. It may be cancelled, " +
                  "unsupported or too short."
              )
            )
            return@launch
          }

          val result = AnalysisResult(flat)
          promise.resolve(
            mapOf(
              "trackId" to trackId,
              "durationMs" to result.durationMs,
              "bpm" to result.bpm,
              "bpmConfidence" to result.bpmConfidence,
              "alternateBpm" to result.alternateBpm,
              "beatConfidence" to result.beatConfidence,
              "downbeatConfidence" to result.downbeatConfidence,
              "beatsPerBar" to result.beatsPerBar,
              "keyTonic" to result.keyTonic,
              "keyMode" to result.keyMode,
              "keyConfidence" to result.keyConfidence,
              "integratedLufs" to result.integratedLufs,
              "peakDbfs" to result.peakDbfs,
              "energy" to result.energy,
              "introEndMs" to result.introEndMs,
              "outroStartMs" to result.outroStartMs,
              "analysisSampleRate" to result.analysisSampleRate,
              "beatCount" to result.beatsMs.size,
              // Full curves stay native-side heavy; only what the UI and the
              // planner need crosses the bridge. Beat grids are written to
              // disk by the caller from `beatsMs` when it needs them.
              "beatsMs" to result.beatsMs.toList(),
              "downbeatIndices" to result.downbeatIndices.toList(),
              "sections" to result.sections.map {
                mapOf(
                  "startMs" to it.startMs,
                  "endMs" to it.endMs,
                  "energy" to it.energy,
                  "novelty" to it.novelty
                )
              }
            )
          )
        } catch (error: CodedException) {
          promise.reject(error)
        } catch (error: Exception) {
          promise.reject(
            AnalysisException("ANALYSIS_FAILED", error.message ?: "Unknown error")
          )
        }
      }
    }

    AsyncFunction("cancel") {
      cancelled.set(true)
      NativeAnalyser.nativeRequestCancel()
      job?.cancel()
    }

    OnDestroy {
      cancelled.set(true)
      NativeAnalyser.nativeRequestCancel()
      job?.cancel()
    }
  }
}
