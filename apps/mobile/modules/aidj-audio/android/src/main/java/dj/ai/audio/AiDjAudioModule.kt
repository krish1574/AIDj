package dj.ai.audio

import android.net.Uri
import expo.modules.kotlin.exception.CodedException
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

class AudioEngineException(code: EngineErrorCode) :
  CodedException(code.wireName, "The audio engine rejected the request.", null)

/**
 * Expo module surface for the audio engine.
 *
 * Every call is a thin pass-through. Native errors become typed exceptions so
 * the JS side gets a real rejection with a stable code rather than a magic
 * integer it has to interpret.
 */
class AiDjAudioModule : Module() {
  private fun check(code: Int) {
    val error = EngineErrorCode.from(code)
    if (error != EngineErrorCode.NONE) throw AudioEngineException(error)
  }

  /**
   * The document picker and MediaStore both hand back content:// URIs, and the
   * NDK media extractor cannot open those - only ContentResolver holds the SAF
   * grant. So we resolve the URI to a descriptor here and pass that down as an
   * fd:// URI. Ownership of the descriptor transfers to native code, which
   * closes it once the extractor has dup'd it; that is why detachFd() is used
   * rather than letting the ParcelFileDescriptor close on scope exit.
   *
   * Anything that is already a path or an http(s) URL is passed through, since
   * the extractor opens those itself.
   */
  private fun resolveForNative(uri: String): String {
    if (!uri.startsWith("content://")) return uri

    val resolver = appContext.reactContext?.contentResolver
      ?: throw AudioEngineException(EngineErrorCode.DECODER_IO_ERROR)

    val descriptor = try {
      resolver.openFileDescriptor(Uri.parse(uri), "r")
    } catch (error: Exception) {
      null
    } ?: throw AudioEngineException(EngineErrorCode.FILE_NOT_FOUND)

    val length = descriptor.statSize
    if (length <= 0) {
      descriptor.close()
      throw AudioEngineException(EngineErrorCode.DECODER_IO_ERROR)
    }

    return "fd://${descriptor.detachFd()}?offset=0&length=$length"
  }

  override fun definition() = ModuleDefinition {
    Name("AiDjAudio")

    Constants(
      "sampleRate" to 48000,
      "channelCount" to 2,
      "voiceCount" to 2
    )

    AsyncFunction("initialise") { check(NativeEngine.nativeInitialise()) }

    AsyncFunction("shutdown") { NativeEngine.nativeShutdown() }

    AsyncFunction("loadVoice") { voiceIndex: Int, uri: String ->
      check(NativeEngine.nativeLoadVoice(voiceIndex, resolveForNative(uri)))
    }

    AsyncFunction("playVoice") { voiceIndex: Int ->
      check(NativeEngine.nativePlayVoice(voiceIndex))
    }

    AsyncFunction("pause") { check(NativeEngine.nativePause()) }

    AsyncFunction("resume") { check(NativeEngine.nativeResume()) }

    AsyncFunction("stopAll") { NativeEngine.nativeStopAll() }

    AsyncFunction("devCrossfade") { fromVoice: Int, toVoice: Int, durationMs: Int ->
      check(NativeEngine.nativeDevCrossfade(fromVoice, toVoice, durationMs))
    }

    /**
     * Loads a voice cued to a position and pre-stretched to a tempo.
     *
     * Separate from loadVoice because a transition needs the incoming track
     * already decoding, already at the matched tempo and already sitting at
     * its cue point before the fade starts.
     */
    AsyncFunction("prepareVoice") {
      voiceIndex: Int, uri: String, startMs: Double, tempoRatio: Double ->
      check(
        NativeEngine.nativePrepareVoice(
          voiceIndex,
          resolveForNative(uri),
          startMs,
          tempoRatio
        )
      )
    }

    /** Arms a transition planned in TypeScript. Field order is fixed. */
    AsyncFunction("armTransition") {
      spec: DoubleArray, outgoingVoice: Int, incomingVoice: Int, delayMs: Double ->
      if (spec.size < TransitionSpecFields.COUNT) {
        throw AudioEngineException(EngineErrorCode.INVALID_STATE)
      }
      check(
        NativeEngine.nativeArmTransition(spec, outgoingVoice, incomingVoice, delayMs)
      )
    }

    AsyncFunction("clearTransition") { NativeEngine.nativeClearTransition() }

    Function("transitionsCompleted") {
      NativeEngine.nativeTransitionsCompleted().toDouble()
    }

    // Synchronous by design: the debug screen polls this on a timer and an
    // async hop per poll would add jitter to the very numbers being observed.
    Function("getStatus") {
      val raw = NativeEngine.nativeStatus()
      val voices = (0 until 2).map { index ->
        val base = 6 + index * 6
        mapOf(
          "index" to index,
          "hasSource" to (raw[base] > 0.5),
          "positionMs" to raw[base + 1],
          "durationMs" to raw[base + 2],
          "gain" to raw[base + 3],
          "primed" to (raw[base + 4] > 0.5),
          "endOfStream" to (raw[base + 5] > 0.5)
        )
      }

      mapOf(
        "state" to EngineStateName.from(raw[0].toInt()).wireName,
        "underrunCount" to raw[1].toInt(),
        "framesPerBurst" to raw[2].toInt(),
        "outputLatencyMs" to if (raw[3] < 0) null else raw[3],
        "starvedFrames" to raw[4].toInt(),
        "lastError" to EngineErrorCode.from(raw[5].toInt()).wireName,
        "voices" to voices
      )
    }

    OnDestroy { NativeEngine.nativeShutdown() }
  }
}
