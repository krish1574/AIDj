#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

#include "analysis/BeatTracker.h"
#include "analysis/Fft.h"
#include "analysis/OnsetEnvelope.h"

using namespace aidj;

namespace {

constexpr int32_t kSampleRate = 48000;

/**
 * A click train at a known tempo: the only rhythm signal whose correct answer
 * we know exactly. If the tracker cannot find the tempo of this, no result on
 * real music means anything.
 *
 * Each click is a short decaying burst of broadband noise plus a low thump, so
 * it excites both the full-band and low-band envelopes the way a kick does.
 */
std::vector<float> clickTrain(double bpm, double seconds, int accentEvery = 0) {
  const size_t total = static_cast<size_t>(seconds * kSampleRate);
  std::vector<float> samples(total, 0.0f);

  const double interval = 60.0 / bpm * kSampleRate;
  const size_t clickLength = static_cast<size_t>(kSampleRate * 0.03);

  uint32_t rng = 12345;
  int beatIndex = 0;
  for (double position = 0.0; position < static_cast<double>(total);
       position += interval, ++beatIndex) {
    const size_t start = static_cast<size_t>(position);
    const bool accent = accentEvery > 0 && (beatIndex % accentEvery) == 0;
    const float gain = accent ? 1.0f : 0.55f;

    for (size_t i = 0; i < clickLength && start + i < total; ++i) {
      const double decay = std::exp(-static_cast<double>(i) / (kSampleRate * 0.006));
      rng = rng * 1664525u + 1013904223u;
      const float noise =
          (static_cast<float>((rng >> 9) & 0x7FFF) / 16383.5f) - 1.0f;
      // Low thump, stronger on accented beats - this is what downbeat
      // detection keys on.
      const double thumpHz = accent ? 55.0 : 70.0;
      const float thump = static_cast<float>(
          std::sin(2.0 * 3.14159265358979 * thumpHz *
                   static_cast<double>(i) / kSampleRate));
      samples[start + i] += static_cast<float>(
          gain * decay * (0.35 * noise + (accent ? 0.9 : 0.5) * thump));
    }
  }
  return samples;
}

std::vector<float> buildEnvelope(const std::vector<float>& samples,
                                 OnsetEnvelope& detector) {
  detector.process(samples.data(), samples.size());
  const std::vector<float> raw = detector.finish();
  return normaliseEnvelope(raw, 32);
}

}  // namespace

TEST_CASE("FFT round-trips a known sinusoid to the right bin", "[analysis]") {
  const size_t size = 2048;
  Fft fft(size);

  // Exactly 64 cycles across the frame lands entirely in bin 64, with no
  // leakage - the cleanest possible check that the transform is correct.
  std::vector<float> input(size);
  for (size_t i = 0; i < size; ++i) {
    input[i] = std::sin(2.0f * 3.14159265f * 64.0f *
                        static_cast<float>(i) / static_cast<float>(size));
  }

  std::vector<float> magnitudes(size / 2 + 1, 0.0f);
  fft.magnitudeSpectrum(input.data(), magnitudes.data());

  size_t peakBin = 0;
  for (size_t i = 0; i < magnitudes.size(); ++i) {
    if (magnitudes[i] > magnitudes[peakBin]) peakBin = i;
  }

  REQUIRE(peakBin == 64);
  // Energy should be concentrated: neighbours far below the peak.
  REQUIRE(magnitudes[63] < magnitudes[64] * 0.05f);
  REQUIRE(magnitudes[65] < magnitudes[64] * 0.05f);
}

TEST_CASE("FFT of silence is silent", "[analysis]") {
  Fft fft(256);
  std::vector<float> input(256, 0.0f);
  std::vector<float> magnitudes(129, 1.0f);
  fft.magnitudeSpectrum(input.data(), magnitudes.data());
  for (float magnitude : magnitudes) REQUIRE(magnitude < 1e-6f);
}

TEST_CASE("onset envelope has no spurious onset at t=0", "[analysis]") {
  // The first frame has no predecessor. Reporting its raw spectrum as flux
  // would plant a huge false onset at the start of every track.
  AnalysisFormat format;
  OnsetEnvelope detector(format);
  const std::vector<float> samples = clickTrain(120.0, 4.0);
  detector.process(samples.data(), samples.size());
  const std::vector<float> raw = detector.finish();

  REQUIRE(raw.size() > 10);
  REQUIRE(raw[0] == 0.0f);
}

TEST_CASE("tempo estimation recovers known BPM", "[analysis]") {
  AnalysisFormat format;

  // Spread across the range the estimator claims to cover, including tempi
  // that are not multiples of each other.
  const double tempos[] = {90.0, 100.0, 120.0, 128.0, 140.0, 174.0};

  for (double expected : tempos) {
    OnsetEnvelope detector(format);
    const std::vector<float> samples = clickTrain(expected, 20.0);
    const std::vector<float> envelope = buildEnvelope(samples, detector);

    const TempoEstimate estimate =
        estimateTempo(envelope, format.framesPerSecond());

    INFO("expected " << expected << " got " << estimate.bpm);
    // Within 2%: finite hop resolution means the estimate is quantised.
    REQUIRE(std::abs(estimate.bpm - expected) / expected < 0.02);
    REQUIRE(estimate.confidence > 0.0f);
  }
}

TEST_CASE("beat grid lands on the actual beats", "[analysis]") {
  AnalysisFormat format;
  OnsetEnvelope detector(format);

  const double bpm = 128.0;
  const std::vector<float> samples = clickTrain(bpm, 20.0);
  const std::vector<float> envelope = buildEnvelope(samples, detector);

  const TempoEstimate tempo = estimateTempo(envelope, format.framesPerSecond());
  const BeatGrid grid =
      trackBeats(envelope, detector.lowBandFlux(), tempo, format);

  REQUIRE(grid.beatsMs.size() > 30);

  const double expectedInterval = 60000.0 / bpm;

  // Every beat should sit near a true click. Allowing 25 ms is generous but
  // meaningful: one STFT hop is ~10.7 ms, so this is about two hops.
  for (double beatMs : grid.beatsMs) {
    const double nearest = std::round(beatMs / expectedInterval) * expectedInterval;
    INFO("beat at " << beatMs << " nearest true beat " << nearest);
    REQUIRE(std::abs(beatMs - nearest) < 25.0);
  }

  // And the grid must be monotonic with a stable interval - a grid that skips
  // or doubles back would still pass a per-beat proximity test.
  for (size_t i = 1; i < grid.beatsMs.size(); ++i) {
    const double interval = grid.beatsMs[i] - grid.beatsMs[i - 1];
    REQUIRE(interval > 0.0);
    REQUIRE(std::abs(interval - expectedInterval) < expectedInterval * 0.25);
  }
}

TEST_CASE("downbeats align with accented bars", "[analysis]") {
  AnalysisFormat format;
  OnsetEnvelope detector(format);

  // Accent every fourth beat, so the correct downbeat phase is unambiguous.
  const std::vector<float> samples = clickTrain(128.0, 24.0, 4);
  const std::vector<float> envelope = buildEnvelope(samples, detector);

  const TempoEstimate tempo = estimateTempo(envelope, format.framesPerSecond());
  const BeatGrid grid =
      trackBeats(envelope, detector.lowBandFlux(), tempo, format);

  REQUIRE(!grid.downbeatIndices.empty());
  REQUIRE(grid.beatsPerBar == 4);

  // Downbeats must be exactly one bar apart in beat indices.
  for (size_t i = 1; i < grid.downbeatIndices.size(); ++i) {
    REQUIRE(grid.downbeatIndices[i] - grid.downbeatIndices[i - 1] == 4);
  }

  // The accent is real and strong, so the detector should be confident.
  REQUIRE(grid.downbeatConfidence > 0.1f);
}

TEST_CASE("beat confidence separates a real pulse from noise", "[analysis]") {
  // The metric must discriminate, not saturate. An earlier version compared
  // on-beat strength against the overall mean and returned 1.0 for every
  // input including speech, which made it useless to the planner that is
  // supposed to distrust weak grids.
  AnalysisFormat format;

  float pulseConfidence = 0.0f;
  {
    OnsetEnvelope detector(format);
    const std::vector<float> samples = clickTrain(128.0, 20.0);
    const std::vector<float> envelope = buildEnvelope(samples, detector);
    const TempoEstimate tempo = estimateTempo(envelope, format.framesPerSecond());
    pulseConfidence =
        trackBeats(envelope, detector.lowBandFlux(), tempo, format)
            .beatConfidence;
  }

  float noiseConfidence = 1.0f;
  {
    OnsetEnvelope detector(format);
    std::vector<float> samples(kSampleRate * 20);
    uint32_t rng = 4321;
    for (float& sample : samples) {
      rng = rng * 1664525u + 1013904223u;
      sample = ((static_cast<float>((rng >> 9) & 0x7FFF) / 16383.5f) - 1.0f) * 0.3f;
    }
    const std::vector<float> envelope = buildEnvelope(samples, detector);
    const TempoEstimate tempo = estimateTempo(envelope, format.framesPerSecond());
    noiseConfidence =
        trackBeats(envelope, detector.lowBandFlux(), tempo, format)
            .beatConfidence;
  }

  INFO("pulse " << pulseConfidence << " noise " << noiseConfidence);
  REQUIRE(pulseConfidence > 0.5f);
  REQUIRE(noiseConfidence < pulseConfidence);
  REQUIRE(noiseConfidence < 0.5f);
}

TEST_CASE("noise yields low tempo confidence", "[analysis]") {
  // White noise has no tempo. The estimator will still return *a* number -
  // autocorrelation always peaks somewhere - so what matters is that it does
  // not claim confidence in it. A planner acting on a confident wrong grid is
  // worse than one told the track is unreadable.
  AnalysisFormat format;
  OnsetEnvelope detector(format);

  std::vector<float> samples(kSampleRate * 20);
  uint32_t rng = 999;
  for (float& sample : samples) {
    rng = rng * 1664525u + 1013904223u;
    sample = ((static_cast<float>((rng >> 9) & 0x7FFF) / 16383.5f) - 1.0f) * 0.3f;
  }

  const std::vector<float> envelope = buildEnvelope(samples, detector);
  const TempoEstimate estimate = estimateTempo(envelope, format.framesPerSecond());

  INFO("noise confidence " << estimate.confidence);
  REQUIRE(estimate.confidence < 0.5f);
}
