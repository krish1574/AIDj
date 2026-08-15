package dj.ai.analysis

/**
 * Raw JNI surface for track analysis.
 *
 * The native side returns one flat double array rather than an object graph:
 * a 50 minute mix produces tens of thousands of beat timestamps and energy
 * samples, and building that many JVM objects across JNI would cost more than
 * the DSP.
 */
object NativeAnalyser {
  init {
    System.loadLibrary("aidj_analysis_jni")
  }

  /** Return false from onProgress to cancel. */
  interface ProgressCallback {
    fun onProgress(decodedMs: Double, totalMs: Double): Boolean
  }

  external fun nativeAnalyse(uri: String, callback: ProgressCallback?): DoubleArray?

  external fun nativeRequestCancel()
}

/** Mirrors the ScalarIndex enum in AnalysisJni.cpp. Order must match exactly. */
private object Scalar {
  const val DURATION_MS = 0
  const val BPM = 1
  const val BPM_CONFIDENCE = 2
  const val ALTERNATE_BPM = 3
  const val BEAT_COUNT = 4
  const val DOWNBEAT_COUNT = 5
  const val BEAT_CONFIDENCE = 6
  const val DOWNBEAT_CONFIDENCE = 7
  const val BEATS_PER_BAR = 8
  const val KEY_TONIC = 9
  const val KEY_MODE = 10
  const val KEY_CONFIDENCE = 11
  const val INTEGRATED_LUFS = 12
  const val PEAK_DBFS = 13
  const val ENERGY = 14
  const val INTRO_END_MS = 15
  const val OUTRO_START_MS = 16
  const val SECTION_COUNT = 17
  const val ENERGY_CURVE_HOP_MS = 18
  const val VOCAL_HOP_MS = 19
  const val ANALYSIS_SAMPLE_RATE = 20
  const val COUNT = 21
}

data class AnalysisSection(
  val startMs: Double,
  val endMs: Double,
  val energy: Double,
  val novelty: Double
)

/**
 * A decoded analysis result.
 *
 * `beatsMs`, `energyCurve` and `vocalActivity` are large. They are kept as
 * primitive arrays and written to disk as binary blobs rather than JSON -
 * a 50 minute mix has ~10k beats and ~280k energy samples, which is the wrong
 * shape for a document store.
 */
class AnalysisResult(private val flat: DoubleArray) {
  val durationMs: Double get() = flat[Scalar.DURATION_MS]
  val bpm: Double get() = flat[Scalar.BPM]
  val bpmConfidence: Double get() = flat[Scalar.BPM_CONFIDENCE]
  val alternateBpm: Double get() = flat[Scalar.ALTERNATE_BPM]
  val beatConfidence: Double get() = flat[Scalar.BEAT_CONFIDENCE]
  val downbeatConfidence: Double get() = flat[Scalar.DOWNBEAT_CONFIDENCE]
  val beatsPerBar: Int get() = flat[Scalar.BEATS_PER_BAR].toInt()
  val keyTonic: Int get() = flat[Scalar.KEY_TONIC].toInt()
  /** 0 = major, 1 = minor. */
  val keyMode: Int get() = flat[Scalar.KEY_MODE].toInt()
  val keyConfidence: Double get() = flat[Scalar.KEY_CONFIDENCE]
  val integratedLufs: Double get() = flat[Scalar.INTEGRATED_LUFS]
  val peakDbfs: Double get() = flat[Scalar.PEAK_DBFS]
  val energy: Double get() = flat[Scalar.ENERGY]
  val introEndMs: Double get() = flat[Scalar.INTRO_END_MS]
  val outroStartMs: Double get() = flat[Scalar.OUTRO_START_MS]
  val energyCurveHopMs: Double get() = flat[Scalar.ENERGY_CURVE_HOP_MS]
  val vocalActivityHopMs: Double get() = flat[Scalar.VOCAL_HOP_MS]
  val analysisSampleRate: Int get() = flat[Scalar.ANALYSIS_SAMPLE_RATE].toInt()

  private val beatCount: Int get() = flat[Scalar.BEAT_COUNT].toInt()
  private val downbeatCount: Int get() = flat[Scalar.DOWNBEAT_COUNT].toInt()
  private val sectionCount: Int get() = flat[Scalar.SECTION_COUNT].toInt()

  private val beatsOffset: Int get() = Scalar.COUNT
  private val downbeatsOffset: Int get() = beatsOffset + beatCount
  private val sectionsOffset: Int get() = downbeatsOffset + downbeatCount
  private val energyOffset: Int get() = sectionsOffset + sectionCount * 4

  val beatsMs: DoubleArray
    get() = flat.copyOfRange(beatsOffset, beatsOffset + beatCount)

  val downbeatIndices: IntArray
    get() = IntArray(downbeatCount) { flat[downbeatsOffset + it].toInt() }

  val sections: List<AnalysisSection>
    get() = (0 until sectionCount).map { index ->
      val base = sectionsOffset + index * 4
      AnalysisSection(flat[base], flat[base + 1], flat[base + 2], flat[base + 3])
    }

  val energyCurve: DoubleArray
    get() {
      // Everything after the sections is the energy curve followed by the
      // vocal curve, and both are sampled at the analysis frame rate, so they
      // are the same length.
      val remaining = flat.size - energyOffset
      val each = remaining / 2
      return flat.copyOfRange(energyOffset, energyOffset + each)
    }

  val vocalActivity: DoubleArray
    get() {
      val remaining = flat.size - energyOffset
      val each = remaining / 2
      return flat.copyOfRange(energyOffset + each, energyOffset + each * 2)
    }
}
