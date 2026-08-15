#include "analysis/OnsetEnvelope.h"

#include <algorithm>
#include <cmath>

namespace aidj {
namespace {

constexpr float kEpsilon = 1e-10f;
constexpr float kLowBandHz = 200.0f;
constexpr float kVocalLowHz = 200.0f;
constexpr float kVocalHighHz = 4000.0f;
constexpr float kChromaMinHz = 55.0f;    // A1
constexpr float kChromaMaxHz = 2093.0f;  // C7

size_t binForHz(float hz, int32_t sampleRate, size_t frameSize) {
  const float nyquist = static_cast<float>(sampleRate) * 0.5f;
  const float clamped = std::min(hz, nyquist);
  const float bins = static_cast<float>(frameSize) * 0.5f;
  return static_cast<size_t>(clamped / nyquist * bins);
}

}  // namespace

OnsetEnvelope::OnsetEnvelope(AnalysisFormat format)
    : format_(format),
      fft_(static_cast<size_t>(format.frameSize)),
      window_(hannWindow(static_cast<size_t>(format.frameSize))),
      frame_(static_cast<size_t>(format.frameSize), 0.0f),
      magnitudes_(static_cast<size_t>(format.frameSize) / 2 + 1, 0.0f),
      previousLogMagnitudes_(static_cast<size_t>(format.frameSize) / 2 + 1,
                             0.0f) {
  buffer_.reserve(static_cast<size_t>(format.frameSize) * 2);

  lowBandMaxBin_ = binForHz(kLowBandHz, format.sampleRate,
                            static_cast<size_t>(format.frameSize));
  vocalMinBin_ = binForHz(kVocalLowHz, format.sampleRate,
                          static_cast<size_t>(format.frameSize));
  vocalMaxBin_ = binForHz(kVocalHighHz, format.sampleRate,
                          static_cast<size_t>(format.frameSize));

  // Bin -> pitch class, computed once. -1 means the bin falls outside the
  // musical range and is skipped.
  const size_t bins = static_cast<size_t>(format.frameSize) / 2 + 1;
  binPitchClass_.assign(bins, -1);
  const float binHz = static_cast<float>(format.sampleRate) /
                      static_cast<float>(format.frameSize);
  for (size_t bin = 1; bin < bins; ++bin) {
    const float hz = static_cast<float>(bin) * binHz;
    if (hz < kChromaMinHz || hz > kChromaMaxHz) continue;
    const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
    binPitchClass_[bin] = static_cast<int8_t>(
        ((static_cast<int>(std::lround(midi)) % 12) + 12) % 12);
  }
}

void OnsetEnvelope::process(const float* samples, size_t count) {
  buffer_.insert(buffer_.end(), samples, samples + count);

  const size_t frameSize = static_cast<size_t>(format_.frameSize);
  const size_t hop = static_cast<size_t>(format_.hopSize);

  size_t offset = 0;
  while (buffer_.size() - offset >= frameSize) {
    for (size_t i = 0; i < frameSize; ++i) {
      frame_[i] = buffer_[offset + i] * window_[i];
    }
    processFrame();
    offset += hop;
  }

  if (offset > 0) buffer_.erase(buffer_.begin(), buffer_.begin() + offset);
}

void OnsetEnvelope::processFrame() {
  fft_.magnitudeSpectrum(frame_.data(), magnitudes_.data());

  const size_t bins = magnitudes_.size();

  float flux = 0.0f;
  float lowFlux = 0.0f;
  float totalEnergy = 0.0f;
  float vocalEnergy = 0.0f;

  for (size_t bin = 0; bin < bins; ++bin) {
    const float magnitude = magnitudes_[bin];
    // Log compression: without it the envelope is dominated by whichever
    // section of the track happens to be loudest.
    const float logMagnitude = std::log1p(magnitude);
    const float delta = logMagnitude - previousLogMagnitudes_[bin];

    // Half-wave rectification - only energy increases are onsets.
    if (delta > 0.0f) {
      flux += delta;
      if (bin <= lowBandMaxBin_) lowFlux += delta;
    }

    const float power = magnitude * magnitude;
    totalEnergy += power;
    if (bin >= vocalMinBin_ && bin <= vocalMaxBin_) vocalEnergy += power;

    previousLogMagnitudes_[bin] = logMagnitude;
  }

  if (!hasPrevious_) {
    // The first frame has no predecessor, so its "flux" is just its own
    // spectrum. Emitting that would plant a huge spurious onset at t=0.
    flux = 0.0f;
    lowFlux = 0.0f;
    hasPrevious_ = true;
  }

  flux_.push_back(flux);
  lowFlux_.push_back(lowFlux);

  const float rms = std::sqrt(totalEnergy / static_cast<float>(bins));
  rmsDb_.push_back(20.0f * std::log10(rms + kEpsilon));

  vocalRatio_.push_back(totalEnergy > kEpsilon ? vocalEnergy / totalEnergy
                                               : 0.0f);

  // Chroma: fold every bin in the musical range onto its pitch class, using
  // the precomputed table. The mapping depends only on sample rate and frame
  // size, so the original per-frame logarithms recomputed the same few hundred
  // results endlessly.
  //
  // Measured honestly: removing them did NOT speed analysis up - it stayed at
  // roughly 8x real time on a Galaxy S24 FE. Decoding and the FFT dominate.
  // The table is kept because it is strictly less work, but anyone chasing
  // analysis speed should profile the decode path and the transform, not this.
  float chroma[12] = {0.0f};
  for (size_t bin = 1; bin < bins; ++bin) {
    const int8_t pitchClass = binPitchClass_[bin];
    if (pitchClass < 0) continue;
    chroma[pitchClass] += magnitudes_[bin];
  }
  for (int i = 0; i < 12; ++i) chroma_.push_back(chroma[i]);
}

std::vector<float> OnsetEnvelope::finish() { return flux_; }

std::vector<float> normaliseEnvelope(const std::vector<float>& envelope,
                                     size_t movingAverageFrames) {
  if (envelope.empty()) return {};

  std::vector<float> result(envelope.size(), 0.0f);

  // Scale by a robust statistic rather than the maximum: a single transient
  // (a clap, a glitch) would otherwise flatten the whole envelope.
  std::vector<float> sorted(envelope);
  std::sort(sorted.begin(), sorted.end());
  const float median = sorted[sorted.size() / 2];
  const float high = sorted[static_cast<size_t>(
      static_cast<double>(sorted.size() - 1) * 0.95)];
  const float scale = std::max(high - median, 1e-6f);

  for (size_t i = 0; i < envelope.size(); ++i) {
    result[i] = (envelope[i] - median) / scale;
  }

  if (movingAverageFrames < 2) return result;

  // Subtract a local mean so a slow loudness build is not mistaken for
  // rhythmic periodicity, then rectify.
  std::vector<float> smoothed(result.size(), 0.0f);
  const int half = static_cast<int>(movingAverageFrames / 2);
  for (int i = 0; i < static_cast<int>(result.size()); ++i) {
    const int from = std::max(0, i - half);
    const int to = std::min(static_cast<int>(result.size()) - 1, i + half);
    float sum = 0.0f;
    for (int j = from; j <= to; ++j) sum += result[j];
    smoothed[i] = sum / static_cast<float>(to - from + 1);
  }

  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = std::max(0.0f, result[i] - smoothed[i]);
  }

  return result;
}

}  // namespace aidj
