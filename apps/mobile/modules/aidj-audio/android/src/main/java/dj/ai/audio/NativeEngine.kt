package dj.ai.audio

/**
 * Raw JNI surface. Nothing above this line should know that JNI exists, and
 * nothing below it should know that React Native exists.
 */
object NativeEngine {
  init {
    System.loadLibrary("aidj_audio_jni")
  }

  external fun nativeInitialise(): Int
  external fun nativeShutdown()
  external fun nativeLoadVoice(voiceIndex: Int, uri: String): Int
  external fun nativePlayVoice(voiceIndex: Int): Int
  external fun nativePause(): Int
  external fun nativeResume(): Int
  external fun nativeStopAll()
  external fun nativeDevCrossfade(fromVoice: Int, toVoice: Int, durationMs: Int): Int

  /** Flat status array; see JniBridge.cpp for the layout. */
  external fun nativeStatus(): DoubleArray
}

/** Mirrors aidj::EngineError. */
enum class EngineErrorCode(val code: Int, val wireName: String) {
  NONE(0, "NONE"),
  OUTPUT_OPEN_FAILED(1, "OUTPUT_OPEN_FAILED"),
  DECODER_UNSUPPORTED_FORMAT(2, "DECODER_UNSUPPORTED_FORMAT"),
  DECODER_IO_ERROR(3, "DECODER_IO_ERROR"),
  FILE_NOT_FOUND(4, "FILE_NOT_FOUND"),
  VOICE_BUSY(5, "VOICE_BUSY"),
  INVALID_STATE(6, "INVALID_STATE");

  companion object {
    fun from(code: Int): EngineErrorCode =
      entries.firstOrNull { it.code == code } ?: INVALID_STATE
  }
}

/** Mirrors aidj::EngineState. */
enum class EngineStateName(val code: Int, val wireName: String) {
  UNINITIALISED(0, "uninitialised"),
  IDLE(1, "idle"),
  PREPARING(2, "preparing"),
  PLAYING(3, "playing"),
  PAUSED(4, "paused"),
  ERROR(5, "error");

  companion object {
    fun from(code: Int): EngineStateName =
      entries.firstOrNull { it.code == code } ?: UNINITIALISED
  }
}
