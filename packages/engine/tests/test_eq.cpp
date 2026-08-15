#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "dsp/ThreeBandEq.h"

using aidj::ThreeBandEq;

namespace {

constexpr int32_t kSampleRate = 48000;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> stereoSine(double hz, size_t frames, float amplitude = 0.5f) {
  std::vector<float> out(frames * 2);
  for (size_t f = 0; f < frames; ++f) {
    const auto value = static_cast<float>(
        amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(f) / kSampleRate));
    out[f * 2] = value;
    out[f * 2 + 1] = value;
  }
  return out;
}

/** RMS of the second half, so filter settling is excluded. */
float settledRms(const std::vector<float>& interleaved) {
  const size_t frames = interleaved.size() / 2;
  const size_t start = frames / 2;
  double sum = 0.0;
  for (size_t f = start; f < frames; ++f) {
    const double sample = interleaved[f * 2];
    sum += sample * sample;
  }
  return static_cast<float>(std::sqrt(sum / static_cast<double>(frames - start)));
}

float gainDb(float processed, float reference) {
  return 20.0f * std::log10((processed + 1e-12f) / (reference + 1e-12f));
}

}  // namespace

TEST_CASE("EQ at neutral leaves samples bit-exact", "[eq]") {
  // The reason this is a shelf/bell design rather than a crossover bank: a
  // transition that is not automating EQ must not colour the audio at all.
  ThreeBandEq eq;
  eq.configure(kSampleRate);
  eq.setBandGains(1.0f, 1.0f, 1.0f);
  eq.snapToTargets();

  const auto input = stereoSine(1000.0, 4096);
  auto buffer = input;
  eq.process(buffer.data(), buffer.size() / 2);

  for (size_t i = 0; i < input.size(); ++i) {
    REQUIRE(buffer[i] == input[i]);
  }
}

TEST_CASE("killing the low band removes bass and leaves treble alone", "[eq]") {
  const auto bass = stereoSine(60.0, 48000);
  const auto treble = stereoSine(8000.0, 48000);

  const float bassReference = settledRms(bass);
  const float trebleReference = settledRms(treble);

  ThreeBandEq bassEq;
  bassEq.configure(kSampleRate);
  bassEq.setBandGains(0.0f, 1.0f, 1.0f);
  bassEq.snapToTargets();
  auto bassBuffer = bass;
  bassEq.process(bassBuffer.data(), bassBuffer.size() / 2);

  ThreeBandEq trebleEq;
  trebleEq.configure(kSampleRate);
  trebleEq.setBandGains(0.0f, 1.0f, 1.0f);
  trebleEq.snapToTargets();
  auto trebleBuffer = treble;
  trebleEq.process(trebleBuffer.data(), trebleBuffer.size() / 2);

  const float bassChange = gainDb(settledRms(bassBuffer), bassReference);
  const float trebleChange = gainDb(settledRms(trebleBuffer), trebleReference);

  INFO("60 Hz " << bassChange << " dB, 8 kHz " << trebleChange << " dB");

  // The bass swap has to actually remove the bass, or two basslines still sum.
  REQUIRE(bassChange < -20.0f);
  // And it must not audibly touch the hats, or the mix goes dull mid-transition.
  REQUIRE(trebleChange > -1.5f);
}

TEST_CASE("killing the high band removes treble and leaves bass alone", "[eq]") {
  const auto bass = stereoSine(60.0, 48000);
  const auto treble = stereoSine(10000.0, 48000);

  ThreeBandEq bassEq;
  bassEq.configure(kSampleRate);
  bassEq.setBandGains(1.0f, 1.0f, 0.0f);
  bassEq.snapToTargets();
  auto bassBuffer = bass;
  bassEq.process(bassBuffer.data(), bassBuffer.size() / 2);

  ThreeBandEq trebleEq;
  trebleEq.configure(kSampleRate);
  trebleEq.setBandGains(1.0f, 1.0f, 0.0f);
  trebleEq.snapToTargets();
  auto trebleBuffer = treble;
  trebleEq.process(trebleBuffer.data(), trebleBuffer.size() / 2);

  REQUIRE(gainDb(settledRms(trebleBuffer), settledRms(treble)) < -20.0f);
  REQUIRE(gainDb(settledRms(bassBuffer), settledRms(bass)) > -1.5f);
}

TEST_CASE("EQ never produces a discontinuity when a band is swept", "[eq]") {
  // A stepped coefficient change is an audible click, and the transition
  // engine sweeps these over seconds. Smoothing is not cosmetic.
  ThreeBandEq eq;
  eq.configure(kSampleRate);
  eq.setBandGains(1.0f, 1.0f, 1.0f);
  eq.snapToTargets();

  auto buffer = stereoSine(200.0, 24000, 0.5f);

  // Process the first half neutral, then demand an instant kill.
  eq.process(buffer.data(), 6000);
  eq.setBandGains(0.0f, 1.0f, 1.0f);
  eq.process(buffer.data() + 6000 * 2, 6000);

  // No sample-to-sample jump larger than the signal itself could produce.
  // A 200 Hz sine at 48 kHz moves at most ~0.013 per sample at this
  // amplitude; anything an order of magnitude beyond that is a click.
  float largestJump = 0.0f;
  for (size_t f = 1; f < 12000; ++f) {
    const float jump = std::abs(buffer[f * 2] - buffer[(f - 1) * 2]);
    largestJump = std::max(largestJump, jump);
  }

  INFO("largest sample-to-sample jump: " << largestJump);
  REQUIRE(largestJump < 0.05f);
}

TEST_CASE("EQ is stable over a long sweep", "[eq]") {
  // Recomputing biquad coefficients per sample while gains move must not let
  // the filter state run away.
  ThreeBandEq eq;
  eq.configure(kSampleRate);
  eq.snapToTargets();

  auto buffer = stereoSine(440.0, 48000, 0.8f);

  for (int block = 0; block < 10; ++block) {
    const float gain = block % 2 == 0 ? 0.0f : 1.0f;
    eq.setBandGains(gain, 1.0f, 1.0f - gain);
    eq.process(buffer.data() + block * 4800 * 2, 4800);
  }

  for (float sample : buffer) {
    REQUIRE(std::isfinite(sample));
    REQUIRE(std::abs(sample) < 4.0f);
  }
}
