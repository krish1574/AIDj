#include "analysis/TrackAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "analysis/BeatTracker.h"
#include "analysis/KeyDetector.h"
#include "analysis/Loudness.h"
#include "analysis/OnsetEnvelope.h"
#include "analysis/Structure.h"

namespace aidj {
namespace {

/** Samples pulled from the decoder per iteration. */
constexpr size_t kDecodeChunkSamples = 16384;

/** Progress is reported about this often, not every chunk. */
constexpr double kProgressIntervalMs = 500.0;

/**
 * Moving-average window for envelope normalisation, in frames.
 * ~1.5 s at the default hop: long enough to remove a loudness build, short
 * enough to preserve individual beats.
 */
constexpr size_t kEnvelopeSmoothingFrames = 140;

}  // namespace

TrackAnalyzer::TrackAnalyzer(AnalysisFormat format) : format_(format) {}

TrackAnalysisResult TrackAnalyzer::analyse(IDecoder& decoder,
                                           const ProgressCallback& onProgress) {
  cancelled_ = false;

  TrackAnalysisResult result;
  result.format = format_;

  const DecodedFormat sourceFormat = decoder.format();
  if (sourceFormat.sampleRate <= 0 || sourceFormat.channelCount <= 0) {
    return result;
  }

  // Analyse at the source rate rather than resampling to a fixed one.
  //
  // Nothing downstream needs a particular sample rate - every stage works in
  // frames and derives seconds from framesPerSecond() - so resampling would
  // only add cost and, with the current cubic interpolator, inject artefacts
  // into the exact measurements being taken. Frame size and hop stay constant
  // in samples, so a 44.1 kHz file analyses at 86 fps instead of 93; that is
  // a resolution difference the algorithms already tolerate.
  format_.sampleRate = sourceFormat.sampleRate;
  result.format = format_;

  OnsetEnvelope onsets(format_);
  LoudnessMeter loudness(format_.sampleRate);

  std::vector<float> interleaved(kDecodeChunkSamples);
  std::vector<float> mono;

  double decodedSamples = 0.0;
  double lastProgressMs = -kProgressIntervalMs;

  while (true) {
    const size_t written = decoder.decode(interleaved.data(), interleaved.size());
    if (written == 0) break;

    const size_t channels = static_cast<size_t>(sourceFormat.channelCount);
    const size_t frames = written / channels;
    if (frames == 0) break;

    // Mono sum. Averaging rather than taking one channel keeps a hard-panned
    // element from vanishing entirely.
    mono.resize(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
      float sum = 0.0f;
      for (size_t channel = 0; channel < channels; ++channel) {
        sum += interleaved[frame * channels + channel];
      }
      mono[frame] = sum / static_cast<float>(channels);
    }

    onsets.process(mono.data(), mono.size());
    loudness.process(mono.data(), mono.size());

    decodedSamples += static_cast<double>(frames);
    const double decodedMs =
        decodedSamples * 1000.0 / static_cast<double>(sourceFormat.sampleRate);

    if (decodedMs - lastProgressMs >= kProgressIntervalMs) {
      lastProgressMs = decodedMs;
      Progress progress;
      progress.decodedMs = decodedMs;
      progress.totalMs = static_cast<double>(sourceFormat.durationMs);
      if (onProgress && !onProgress(progress)) {
        cancelled_ = true;
        return result;
      }
    }
  }

  const double durationMs =
      decodedSamples * 1000.0 / static_cast<double>(sourceFormat.sampleRate);
  result.durationMs = durationMs;

  if (onsets.frameCount() < 8) {
    // Too short to analyse. Returning empty is correct - inventing a BPM for a
    // two-second file would be exactly the fake functionality we refuse to ship.
    return result;
  }

  const std::vector<float> envelope =
      normaliseEnvelope(onsets.finish(), kEnvelopeSmoothingFrames);
  const std::vector<float> lowEnvelope =
      normaliseEnvelope(onsets.lowBandFlux(), kEnvelopeSmoothingFrames);

  result.tempo = estimateTempo(envelope, format_.framesPerSecond());
  result.beats = trackBeats(envelope, lowEnvelope, result.tempo, format_);

  const KeyEstimate key = estimateKey(onsets.chromaFrames());
  result.key = key.key;
  result.keyConfidence = key.confidence;

  result.loudness = loudness.finish();

  const StructureResult structure =
      analyseStructure(onsets.chromaFrames(), onsets.frameRmsDb(), format_);
  result.sections = structure.sections;
  result.introEndMs = structure.introEndMs;
  result.outroStartMs = structure.outroStartMs;
  result.energyCurve = structure.energyCurve;
  result.energyCurveHopMs = structure.energyCurveHopMs;
  result.energy = structure.overallEnergy;

  // Vocal activity. This is a heuristic on the proportion of energy in the
  // 200 Hz - 4 kHz band, smoothed. It is NOT stem separation: a bright synth
  // lead reads as vocal, and a vocal buried in a dense mix may not. It is
  // weighted low in transition scoring for exactly this reason.
  const std::vector<float>& ratio = onsets.vocalBandRatio();
  result.vocalActivity.reserve(ratio.size());
  const size_t smoothing = 16;
  for (size_t i = 0; i < ratio.size(); ++i) {
    const size_t from = i >= smoothing ? i - smoothing : 0;
    const size_t to = std::min(ratio.size(), i + smoothing + 1);
    const double sum =
        std::accumulate(ratio.begin() + static_cast<long>(from),
                        ratio.begin() + static_cast<long>(to), 0.0);
    result.vocalActivity.push_back(
        static_cast<float>(std::clamp(sum / static_cast<double>(to - from), 0.0, 1.0)));
  }
  result.vocalActivityHopMs = 1000.0 / format_.framesPerSecond();

  return result;
}

}  // namespace aidj
