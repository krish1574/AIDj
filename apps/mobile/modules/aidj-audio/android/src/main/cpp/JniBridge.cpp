#include <jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "Engine.h"
#include "platform/android/OboeOutput.h"

/**
 * JNI marshalling only. No audio logic lives here - if a decision about
 * mixing, timing or state belongs anywhere, it belongs in the engine, which is
 * portable and host-testable. This file exists purely to move values across
 * the JVM boundary.
 */

namespace {

aidj::Engine& engine() {
  // Deliberately leaked at process exit rather than destroyed: running the
  // engine destructor during Android's teardown races the audio thread for no
  // benefit, since the process is going away regardless.
  static aidj::Engine* instance = new aidj::Engine();
  return *instance;
}

std::string toStdString(JNIEnv* env, jstring value) {
  if (value == nullptr) return {};
  const char* chars = env->GetStringUTFChars(value, nullptr);
  std::string result(chars == nullptr ? "" : chars);
  if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
  return result;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_dj_ai_audio_NativeEngine_nativeInitialise(JNIEnv*, jobject) {
  return static_cast<jint>(
      engine().initialise(std::make_unique<aidj::OboeOutput>()));
}

JNIEXPORT void JNICALL
Java_dj_ai_audio_NativeEngine_nativeShutdown(JNIEnv*, jobject) {
  engine().shutdown();
}

JNIEXPORT jint JNICALL Java_dj_ai_audio_NativeEngine_nativeLoadVoice(
    JNIEnv* env, jobject, jint voiceIndex, jstring uri) {
  return static_cast<jint>(
      engine().loadVoice(voiceIndex, toStdString(env, uri)));
}

JNIEXPORT jint JNICALL
Java_dj_ai_audio_NativeEngine_nativePlayVoice(JNIEnv*, jobject, jint voiceIndex) {
  return static_cast<jint>(engine().playVoice(voiceIndex));
}

JNIEXPORT jint JNICALL Java_dj_ai_audio_NativeEngine_nativePause(JNIEnv*, jobject) {
  return static_cast<jint>(engine().pause());
}

JNIEXPORT jint JNICALL Java_dj_ai_audio_NativeEngine_nativeResume(JNIEnv*, jobject) {
  return static_cast<jint>(engine().resume());
}

JNIEXPORT void JNICALL Java_dj_ai_audio_NativeEngine_nativeStopAll(JNIEnv*, jobject) {
  engine().stopAll();
}

JNIEXPORT jint JNICALL Java_dj_ai_audio_NativeEngine_nativeDevCrossfade(
    JNIEnv*, jobject, jint fromVoice, jint toVoice, jint durationMs) {
  return static_cast<jint>(
      engine().devCrossfade(fromVoice, toVoice, durationMs));
}

JNIEXPORT jint JNICALL Java_dj_ai_audio_NativeEngine_nativePrepareVoice(
    JNIEnv* env, jobject, jint voiceIndex, jstring uri, jdouble startMs,
    jdouble tempoRatio) {
  return static_cast<jint>(engine().prepareVoice(
      voiceIndex, toStdString(env, uri), startMs, tempoRatio));
}

/**
 * Arms a planned transition.
 *
 * The spec crosses as a flat double array rather than a Java object, matching
 * how status is returned: the layout is documented once in TransitionFields
 * below and mirrored in NativeEngine.kt. Building an object graph for
 * something written once per transition would be needless ceremony, and the
 * flat form is what the C++ struct already is.
 */
JNIEXPORT jint JNICALL Java_dj_ai_audio_NativeEngine_nativeArmTransition(
    JNIEnv* env, jobject, jdoubleArray specArray, jint outgoingVoice,
    jint incomingVoice, jdouble delayMs) {
  enum TransitionFields {
    kDurationMs = 0,
    kIncomingStartMs,
    kOutgoingGain,
    kIncomingGain,
    kOutgoingTempoRatio,
    kIncomingTempoRatio,
    kOutgoingLowFrom,
    kOutgoingLowTo,
    kOutgoingMidFrom,
    kOutgoingMidTo,
    kIncomingLowFrom,
    kIncomingLowTo,
    kIncomingMidFrom,
    kIncomingMidTo,
    kOverlap,
    kFieldCount,
  };

  if (env->GetArrayLength(specArray) < kFieldCount) {
    return static_cast<jint>(aidj::EngineError::InvalidState);
  }

  jdouble values[kFieldCount];
  env->GetDoubleArrayRegion(specArray, 0, kFieldCount, values);

  aidj::TransitionSpec spec;
  spec.durationMs = values[kDurationMs];
  spec.incomingStartMs = values[kIncomingStartMs];
  spec.outgoingGain = static_cast<float>(values[kOutgoingGain]);
  spec.incomingGain = static_cast<float>(values[kIncomingGain]);
  spec.outgoingTempoRatio = values[kOutgoingTempoRatio];
  spec.incomingTempoRatio = values[kIncomingTempoRatio];
  spec.outgoingLowFrom = static_cast<float>(values[kOutgoingLowFrom]);
  spec.outgoingLowTo = static_cast<float>(values[kOutgoingLowTo]);
  spec.outgoingMidFrom = static_cast<float>(values[kOutgoingMidFrom]);
  spec.outgoingMidTo = static_cast<float>(values[kOutgoingMidTo]);
  spec.incomingLowFrom = static_cast<float>(values[kIncomingLowFrom]);
  spec.incomingLowTo = static_cast<float>(values[kIncomingLowTo]);
  spec.incomingMidFrom = static_cast<float>(values[kIncomingMidFrom]);
  spec.incomingMidTo = static_cast<float>(values[kIncomingMidTo]);
  spec.overlap = values[kOverlap] > 0.5;

  return static_cast<jint>(
      engine().armTransition(spec, outgoingVoice, incomingVoice, delayMs));
}

JNIEXPORT void JNICALL
Java_dj_ai_audio_NativeEngine_nativeClearTransition(JNIEnv*, jobject) {
  engine().clearTransition();
}

JNIEXPORT jlong JNICALL
Java_dj_ai_audio_NativeEngine_nativeTransitionsCompleted(JNIEnv*, jobject) {
  return static_cast<jlong>(engine().transitionsCompleted());
}

/**
 * Status is returned as a flat double array rather than a JNI-constructed
 * object graph: it is polled a few times a second and allocating a Java object
 * tree per poll is needless GC pressure. The Kotlin side names the fields.
 */
JNIEXPORT jdoubleArray JNICALL
Java_dj_ai_audio_NativeEngine_nativeStatus(JNIEnv* env, jobject) {
  const aidj::EngineStatus status = engine().status();

  constexpr jsize kFieldCount = 6;
  constexpr jsize kPerVoiceFieldCount = 6;
  const jsize length = kFieldCount + kPerVoiceFieldCount * aidj::kVoiceCount;

  jdouble values[kFieldCount + kPerVoiceFieldCount * aidj::kVoiceCount];
  values[0] = static_cast<jdouble>(status.state);
  values[1] = static_cast<jdouble>(status.underrunCount);
  values[2] = static_cast<jdouble>(status.framesPerBurst);
  values[3] = status.outputLatencyMs;
  values[4] = static_cast<jdouble>(status.starvedFrames);
  values[5] = static_cast<jdouble>(status.lastError);

  for (int i = 0; i < aidj::kVoiceCount; ++i) {
    const auto& voice = status.voices[static_cast<size_t>(i)];
    const jsize base = kFieldCount + i * kPerVoiceFieldCount;
    values[base + 0] = voice.hasSource ? 1.0 : 0.0;
    values[base + 1] = static_cast<jdouble>(voice.positionMs);
    values[base + 2] = static_cast<jdouble>(voice.durationMs);
    values[base + 3] = static_cast<jdouble>(voice.gain);
    values[base + 4] = voice.primed ? 1.0 : 0.0;
    values[base + 5] = voice.endOfStream ? 1.0 : 0.0;
  }

  jdoubleArray result = env->NewDoubleArray(length);
  if (result == nullptr) return nullptr;
  env->SetDoubleArrayRegion(result, 0, length, values);
  return result;
}

}  // extern "C"
