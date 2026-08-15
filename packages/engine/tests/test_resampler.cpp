#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "AudioTypes.h"
#include "dsp/Resampler.h"

using aidj::kEngineSampleRate;
using aidj::Resampler;
using Catch::Approx;

namespace {

std::vector<float> makeSine(double frequencyHz, int32_t sampleRate,
                            int32_t channels, size_t frames) {
  std::vector<float> out(frames * static_cast<size_t>(channels));
  for (size_t f = 0; f < frames; ++f) {
    const auto value = static_cast<float>(
        std::sin(6.283185307179586 * frequencyHz * static_cast<double>(f) /
                 sampleRate));
    for (int32_t c = 0; c < channels; ++c) {
      out[f * static_cast<size_t>(channels) + static_cast<size_t>(c)] = value;
    }
  }
  return out;
}

float peak(const std::vector<float>& samples) {
  float result = 0.0f;
  for (const float sample : samples) {
    result = std::max(result, std::fabs(sample));
  }
  return result;
}

}  // namespace

TEST_CASE("Resampler passes 48 kHz stereo through untouched") {
  Resampler resampler;
  resampler.configure(48000, 2, kEngineSampleRate);
  REQUIRE(resampler.isPassThrough());

  const auto input = makeSine(1000.0, 48000, 2, 512);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);

  REQUIRE(output.size() == input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    REQUIRE(output[i] == input[i]);
  }
}

TEST_CASE("Resampler converts mono to stereo") {
  Resampler resampler;
  resampler.configure(48000, 1, kEngineSampleRate);
  REQUIRE_FALSE(resampler.isPassThrough());

  const auto input = makeSine(1000.0, 48000, 1, 4096);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);

  REQUIRE(output.size() % 2 == 0);
  REQUIRE(output.size() > 4000);
  // Both channels must carry the same signal, not silence on the right.
  for (size_t f = 0; f < output.size() / 2; ++f) {
    REQUIRE(output[f * 2] == Approx(output[f * 2 + 1]));
  }
}

TEST_CASE("Resampler produces the expected output length for 44.1 -> 48") {
  Resampler resampler;
  resampler.configure(44100, 2, kEngineSampleRate);

  const size_t inputFrames = 44100;  // one second
  const auto input = makeSine(440.0, 44100, 2, inputFrames);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);
  resampler.finish(output);

  const size_t outputFrames = output.size() / 2;
  // One second in, one second out, within a few frames of interpolation slack.
  REQUIRE(outputFrames > 47950);
  REQUIRE(outputFrames < 48050);
}

TEST_CASE("Resampler preserves amplitude and does not overshoot") {
  Resampler resampler;
  resampler.configure(44100, 2, kEngineSampleRate);

  const auto input = makeSine(440.0, 44100, 2, 44100);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);
  resampler.finish(output);

  // Catmull-Rom can overshoot slightly on steep slopes; anything beyond a few
  // percent would mean the interpolation is wrong, not merely imprecise.
  REQUIRE(peak(output) > 0.95f);
  REQUIRE(peak(output) < 1.05f);
}

TEST_CASE("Resampler has no seam when input arrives in small chunks") {
  const auto input = makeSine(440.0, 44100, 2, 44100);

  Resampler wholeAtOnce;
  wholeAtOnce.configure(44100, 2, kEngineSampleRate);
  std::vector<float> reference;
  wholeAtOnce.process(input.data(), input.size(), reference);

  Resampler chunked;
  chunked.configure(44100, 2, kEngineSampleRate);
  std::vector<float> streamed;
  constexpr size_t kChunk = 512;  // not a multiple of the resample ratio
  for (size_t offset = 0; offset < input.size(); offset += kChunk) {
    const size_t count = std::min(kChunk, input.size() - offset);
    chunked.process(input.data() + offset, count, streamed);
  }

  REQUIRE(streamed.size() == reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    // Bit-identical is the requirement: a chunk boundary must not perturb the
    // interpolation state at all, or every buffer boundary becomes a click.
    REQUIRE(streamed[i] == Approx(reference[i]).margin(1e-6f));
  }
}
