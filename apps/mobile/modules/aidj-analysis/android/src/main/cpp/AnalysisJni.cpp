#include <jni.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "IDecoder.h"
#include "analysis/AnalysisTypes.h"
#include "analysis/TrackAnalyzer.h"

/**
 * JNI marshalling for track analysis. No DSP here - the analyser is portable
 * C++ that is tested on-device via scripts/engine-test-device.mjs, and this
 * file only moves values across the JVM boundary.
 *
 * Analysis runs on whatever thread Kotlin calls from, which is always a
 * background thread. It is deliberately cancellable: a 50 minute mix takes
 * real time, and the worker must be able to stop when the device gets hot or
 * the user leaves.
 */

namespace {

/**
 * Set by Kotlin to request cancellation. A plain atomic flag rather than a
 * per-call token because only one analysis runs at a time by construction -
 * the worker is single-threaded, so parallel analyses would only contend for
 * the same CPU and thermal budget.
 */
std::atomic<bool> gCancelRequested{false};

/** Result layout shared with AnalysisResult.kt. Index-based to avoid building
 *  a Java object graph across JNI for something read once. */
enum ScalarIndex {
  kDurationMs = 0,
  kBpm,
  kBpmConfidence,
  kAlternateBpm,
  kBeatCount,
  kDownbeatCount,
  kBeatConfidence,
  kDownbeatConfidence,
  kBeatsPerBar,
  kKeyTonic,
  kKeyMode,
  kKeyConfidence,
  kIntegratedLufs,
  kPeakDbfs,
  kEnergy,
  kIntroEndMs,
  kOutroStartMs,
  kSectionCount,
  kEnergyCurveHopMs,
  kVocalHopMs,
  kAnalysisSampleRate,
  kScalarCount,
};

}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_dj_ai_analysis_NativeAnalyser_nativeRequestCancel(JNIEnv*, jobject) {
  gCancelRequested.store(true, std::memory_order_relaxed);
}

/**
 * Analyses one already-opened decoder.
 *
 * The decoder is created here rather than passed in because IDecoder is a C++
 * type; Kotlin supplies the file descriptor, matching how playback resolves
 * content:// URIs (see AiDjAudioModule.resolveForNative).
 */
JNIEXPORT jdoubleArray JNICALL Java_dj_ai_analysis_NativeAnalyser_nativeAnalyse(
    JNIEnv* env, jobject self, jstring uri, jobject progressCallback) {
  gCancelRequested.store(false, std::memory_order_relaxed);

  const char* chars = env->GetStringUTFChars(uri, nullptr);
  const std::string path(chars == nullptr ? "" : chars);
  if (chars != nullptr) env->ReleaseStringUTFChars(uri, chars);

  auto decoder = aidj::createPlatformDecoder();
  if (decoder == nullptr) return nullptr;
  if (decoder->open(path) != aidj::EngineError::None) return nullptr;

  // Progress callback into Kotlin. Resolved once rather than per call - a
  // JNI method lookup inside a loop that runs every 500 ms of audio would be
  // wasteful, and more importantly GetMethodID can throw.
  jmethodID onProgress = nullptr;
  if (progressCallback != nullptr) {
    jclass callbackClass = env->GetObjectClass(progressCallback);
    onProgress = env->GetMethodID(callbackClass, "onProgress", "(DD)Z");
    env->DeleteLocalRef(callbackClass);
  }

  aidj::TrackAnalyzer analyzer;
  const aidj::TrackAnalysisResult result = analyzer.analyse(
      *decoder, [&](const aidj::TrackAnalyzer::Progress& progress) {
        if (gCancelRequested.load(std::memory_order_relaxed)) return false;
        if (onProgress == nullptr) return true;
        return static_cast<bool>(env->CallBooleanMethod(
            progressCallback, onProgress, progress.decodedMs, progress.totalMs));
      });

  if (analyzer.wasCancelled()) return nullptr;
  // An empty beat grid on a track long enough to have one means analysis
  // genuinely failed. Reporting null is honest; fabricating a tempo is not.
  if (result.durationMs <= 0.0) return nullptr;

  // Flat layout: [scalars][beats][downbeats][sections x4][energy][vocal]
  const size_t beatCount = result.beats.beatsMs.size();
  const size_t downbeatCount = result.beats.downbeatIndices.size();
  const size_t sectionCount = result.sections.size();
  const size_t energyCount = result.energyCurve.size();
  const size_t vocalCount = result.vocalActivity.size();

  const size_t total = kScalarCount + beatCount + downbeatCount +
                       sectionCount * 4 + energyCount + vocalCount;

  std::vector<double> flat(total, 0.0);

  flat[kDurationMs] = result.durationMs;
  flat[kBpm] = result.tempo.bpm;
  flat[kBpmConfidence] = result.tempo.confidence;
  flat[kAlternateBpm] = result.tempo.alternateBpm;
  flat[kBeatCount] = static_cast<double>(beatCount);
  flat[kDownbeatCount] = static_cast<double>(downbeatCount);
  flat[kBeatConfidence] = result.beats.beatConfidence;
  flat[kDownbeatConfidence] = result.beats.downbeatConfidence;
  flat[kBeatsPerBar] = result.beats.beatsPerBar;
  flat[kKeyTonic] = result.key.tonic;
  flat[kKeyMode] = result.key.mode == aidj::MusicalMode::Major ? 0.0 : 1.0;
  flat[kKeyConfidence] = result.keyConfidence;
  flat[kIntegratedLufs] = result.loudness.integratedLufs;
  flat[kPeakDbfs] = result.loudness.peakDbfs;
  flat[kEnergy] = result.energy;
  flat[kIntroEndMs] = result.introEndMs;
  flat[kOutroStartMs] = result.outroStartMs;
  flat[kSectionCount] = static_cast<double>(sectionCount);
  flat[kEnergyCurveHopMs] = result.energyCurveHopMs;
  flat[kVocalHopMs] = result.vocalActivityHopMs;
  flat[kAnalysisSampleRate] = result.format.sampleRate;

  size_t cursor = kScalarCount;
  for (double beat : result.beats.beatsMs) flat[cursor++] = beat;
  for (int32_t index : result.beats.downbeatIndices) {
    flat[cursor++] = static_cast<double>(index);
  }
  for (const aidj::Section& section : result.sections) {
    flat[cursor++] = section.startMs;
    flat[cursor++] = section.endMs;
    flat[cursor++] = section.energy;
    flat[cursor++] = section.novelty;
  }
  for (float value : result.energyCurve) flat[cursor++] = value;
  for (float value : result.vocalActivity) flat[cursor++] = value;

  jdoubleArray output = env->NewDoubleArray(static_cast<jsize>(total));
  if (output == nullptr) return nullptr;
  env->SetDoubleArrayRegion(output, 0, static_cast<jsize>(total), flat.data());
  return output;
}

}  // extern "C"
