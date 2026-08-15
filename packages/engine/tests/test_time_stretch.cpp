#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "analysis/Fft.h"
#include "dsp/TimeStretch.h"

using aidj::TimeStretch;

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

/** Dominant frequency of the left channel, via the engine's own FFT. */
double dominantFrequency(const std::vector<float>& interleaved, size_t startFrame) {
  constexpr size_t kFftSize = 8192;
  const size_t frames = interleaved.size() / 2;
  REQUIRE(frames > startFrame + kFftSize);

  aidj::Fft fft(kFftSize);
  const std::vector<float> window = aidj::hannWindow(kFftSize);

  std::vector<float> frame(kFftSize);
  for (size_t i = 0; i < kFftSize; ++i) {
    frame[i] = interleaved[(startFrame + i) * 2] * window[i];
  }

  std::vector<float> magnitudes(kFftSize / 2 + 1, 0.0f);
  fft.magnitudeSpectrum(frame.data(), magnitudes.data());

  size_t peak = 1;
  for (size_t bin = 1; bin < magnitudes.size(); ++bin) {
    if (magnitudes[bin] > magnitudes[peak]) peak = bin;
  }
  return static_cast<double>(peak) * kSampleRate / static_cast<double>(kFftSize);
}

float peakOf(const std::vector<float>& samples) {
  float result = 0.0f;
  for (float sample : samples) result = std::max(result, std::abs(sample));
  return result;
}

}  // namespace

TEST_CASE("time stretch at ratio 1 is a pass-through", "[stretch]") {
  TimeStretch stretch;
  stretch.configure(kSampleRate, 2);
  stretch.setRatio(1.0);
  REQUIRE(stretch.isPassThrough());

  const auto input = stereoSine(440.0, 4096);
  std::vector<float> output;
  stretch.process(input.data(), input.size() / 2, output);

  REQUIRE(output.size() == input.size());
  for (size_t i = 0; i < input.size(); ++i) REQUIRE(output[i] == input[i]);
}

TEST_CASE("time stretch changes duration by the ratio", "[stretch]") {
  // 5% faster should produce about 5% fewer samples. This is the property the
  // whole beat-match depends on: if the length is wrong, the grids drift apart.
  for (const double ratio : {0.95, 1.0521, 1.08}) {
    TimeStretch stretch;
    stretch.configure(kSampleRate, 2);
    stretch.setRatio(ratio);

    const size_t inputFrames = kSampleRate * 4;
    const auto input = stereoSine(440.0, inputFrames);
    std::vector<float> output;
    stretch.process(input.data(), inputFrames, output);
    stretch.finish(output);

    const double outputFrames = static_cast<double>(output.size() / 2);
    const double expected = static_cast<double>(inputFrames) / ratio;
    const double error = std::abs(outputFrames - expected) / expected;

    INFO("ratio " << ratio << ": expected " << expected << " got " << outputFrames);
    // Within 2%: WSOLA works in whole windows, so the tail is quantised.
    REQUIRE(error < 0.02);
  }
}

TEST_CASE("time stretch preserves pitch", "[stretch]") {
  // The entire point. Resampling would change duration too, but it would drag
  // the pitch with it and two tracks would clash harmonically.
  TimeStretch stretch;
  stretch.configure(kSampleRate, 2);
  stretch.setRatio(1.06);

  const auto input = stereoSine(440.0, kSampleRate * 4);
  std::vector<float> output;
  stretch.process(input.data(), input.size() / 2, output);
  stretch.finish(output);

  const double frequency = dominantFrequency(output, kSampleRate / 2);
  INFO("dominant frequency after 6% stretch: " << frequency << " Hz");

  // Within one FFT bin (~5.9 Hz) of the original. Naive resampling would put
  // this at 466 Hz - almost a semitone sharp.
  REQUIRE(std::abs(frequency - 440.0) < 8.0);
}

TEST_CASE("time stretch does not overshoot or introduce silence", "[stretch]") {
  // Overlap-add with a bad window dips in level at every splice; a raised
  // cosine crossing at equal power should not.
  TimeStretch stretch;
  stretch.configure(kSampleRate, 2);
  stretch.setRatio(1.04);

  const auto input = stereoSine(220.0, kSampleRate * 3, 0.7f);
  std::vector<float> output;
  stretch.process(input.data(), input.size() / 2, output);
  stretch.finish(output);

  REQUIRE(peakOf(output) > 0.6f);
  REQUIRE(peakOf(output) < 0.85f);

  // No silent gaps in the steady state.
  const size_t frames = output.size() / 2;
  for (size_t f = kSampleRate / 2; f + kSampleRate / 2 < frames; f += 1024) {
    float blockPeak = 0.0f;
    for (size_t i = 0; i < 1024 && f + i < frames; ++i) {
      blockPeak = std::max(blockPeak, std::abs(output[(f + i) * 2]));
    }
    REQUIRE(blockPeak > 0.1f);
  }
}

TEST_CASE("time stretch is unaffected by input chunking", "[stretch]") {
  const auto input = stereoSine(440.0, kSampleRate * 2);

  TimeStretch whole;
  whole.configure(kSampleRate, 2);
  whole.setRatio(1.05);
  std::vector<float> reference;
  whole.process(input.data(), input.size() / 2, reference);

  TimeStretch chunked;
  chunked.configure(kSampleRate, 2);
  chunked.setRatio(1.05);
  std::vector<float> streamed;
  constexpr size_t kChunkFrames = 577;  // deliberately not a window multiple
  for (size_t offset = 0; offset < input.size() / 2; offset += kChunkFrames) {
    const size_t count = std::min(kChunkFrames, input.size() / 2 - offset);
    chunked.process(input.data() + offset * 2, count, streamed);
  }

  REQUIRE(streamed.size() == reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    REQUIRE(std::abs(streamed[i] - reference[i]) < 1e-5f);
  }
}

TEST_CASE("time stretch clamps absurd ratios rather than misbehaving", "[stretch]") {
  TimeStretch stretch;
  stretch.configure(kSampleRate, 2);

  stretch.setRatio(100.0);
  REQUIRE(stretch.ratio() <= 2.0);

  stretch.setRatio(0.0);
  REQUIRE(stretch.ratio() >= 0.5);
}
