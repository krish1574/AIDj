#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "AudioTypes.h"
#include "mix/Mixer.h"

using aidj::Command;
using aidj::CommandType;
using aidj::GainRamp;
using aidj::kEngineChannelCount;
using aidj::Mixer;
using Catch::Approx;

namespace {

std::vector<float> renderBlocks(Mixer& mixer, int32_t frames, int blocks) {
  std::vector<float> all;
  std::vector<float> buffer(static_cast<size_t>(frames) * kEngineChannelCount);
  for (int i = 0; i < blocks; ++i) {
    mixer.render(buffer.data(), frames);
    all.insert(all.end(), buffer.begin(), buffer.end());
  }
  return all;
}

/** Peak absolute value across an interleaved block. */
float peak(const std::vector<float>& samples) {
  float result = 0.0f;
  for (const float sample : samples) result = std::max(result, std::fabs(sample));
  return result;
}

}  // namespace

TEST_CASE("GainRamp is linear when asked to be") {
  const GainRamp ramp{0, 100, 0.0f, 1.0f, false};
  REQUIRE(ramp.valueAt(0) == Approx(0.0f));
  REQUIRE(ramp.valueAt(50) == Approx(0.5f));
  REQUIRE(ramp.valueAt(100) == Approx(1.0f));
}

TEST_CASE("GainRamp clamps outside its window") {
  const GainRamp ramp{100, 200, 0.0f, 1.0f, false};
  REQUIRE(ramp.valueAt(0) == Approx(0.0f));
  REQUIRE(ramp.valueAt(500) == Approx(1.0f));
}

TEST_CASE("Equal-power crossfade holds constant summed power") {
  const GainRamp out{0, 1000, 1.0f, 0.0f, true};
  const GainRamp in{0, 1000, 0.0f, 1.0f, true};

  // The whole point of equal power: two uncorrelated sources sum to constant
  // power through the fade instead of dipping ~3 dB in the middle.
  for (int64_t frame = 0; frame <= 1000; frame += 50) {
    const float a = out.valueAt(frame);
    const float b = in.valueAt(frame);
    REQUIRE(a * a + b * b == Approx(1.0f).margin(0.001f));
  }
}

TEST_CASE("A linear crossfade would dip - proving the test can fail") {
  const GainRamp out{0, 1000, 1.0f, 0.0f, false};
  const GainRamp in{0, 1000, 0.0f, 1.0f, false};
  const float a = out.valueAt(500);
  const float b = in.valueAt(500);
  REQUIRE(a * a + b * b == Approx(0.5f).margin(0.001f));
}

TEST_CASE("Mixer outputs silence when no voice is active") {
  Mixer mixer;
  const auto output = renderBlocks(mixer, 128, 4);
  REQUIRE(peak(output) == 0.0f);
}

TEST_CASE("Mixer advances its frame clock by exactly what it rendered") {
  Mixer mixer;
  renderBlocks(mixer, 128, 5);
  REQUIRE(mixer.frameClock() == 640);
}

TEST_CASE("Mixer counts starvation when an active voice has no data") {
  Mixer mixer;
  mixer.post(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0});
  mixer.post(Command{CommandType::SetVoiceGain, 0, 0, 1.0f, 0});
  renderBlocks(mixer, 128, 2);

  // The voice is active and its ring is empty, so every frame is starved.
  REQUIRE(mixer.starvedFrames() == 256);
}

TEST_CASE("Mixer sums two voices and limits below the ceiling") {
  Mixer mixer;

  // Fill both rings with full-scale DC so the naive sum would be 2.0.
  const size_t frames = 4096;
  std::vector<float> loud(frames * kEngineChannelCount, 1.0f);
  mixer.voice(0).ring.write(loud.data(), loud.size());
  mixer.voice(1).ring.write(loud.data(), loud.size());

  mixer.post(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0});
  mixer.post(Command{CommandType::SetVoiceActive, 1, 0, 1.0f, 0});
  mixer.post(Command{CommandType::SetVoiceGain, 0, 0, 1.0f, 0});
  mixer.post(Command{CommandType::SetVoiceGain, 1, 0, 1.0f, 0});

  const auto output = renderBlocks(mixer, 512, 4);

  REQUIRE(peak(output) > 0.5f);
  // Never clips. This is the safety property that matters most.
  REQUIRE(peak(output) <= aidj::kOutputCeiling);
}

TEST_CASE("Dev crossfade moves gain from one voice to the other") {
  Mixer mixer;

  const size_t frames = 48000;
  std::vector<float> dc(frames * kEngineChannelCount, 0.5f);
  mixer.voice(0).ring.write(dc.data(), dc.size());
  mixer.voice(1).ring.write(dc.data(), dc.size());

  mixer.post(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0});
  mixer.post(Command{CommandType::SetVoiceGain, 0, 0, 1.0f, 0});
  renderBlocks(mixer, 256, 1);
  REQUIRE(mixer.voice(0).currentGain.load() == Approx(1.0f));
  REQUIRE(mixer.voice(1).currentGain.load() == Approx(0.0f));

  // 4800 frames = 100 ms at 48 kHz.
  mixer.post(Command{CommandType::DevCrossfade, 0, 1, 0.0f, 4800});
  renderBlocks(mixer, 256, 10);  // halfway-ish

  const float midOut = mixer.voice(0).currentGain.load();
  const float midIn = mixer.voice(1).currentGain.load();
  REQUIRE(midOut < 1.0f);
  REQUIRE(midIn > 0.0f);

  renderBlocks(mixer, 256, 20);  // well past the end
  REQUIRE(mixer.voice(0).currentGain.load() == Approx(0.0f).margin(0.001f));
  REQUIRE(mixer.voice(1).currentGain.load() == Approx(1.0f).margin(0.001f));
}

TEST_CASE("Command queue drops rather than blocking when full") {
  aidj::CommandQueue queue;
  int accepted = 0;
  for (int i = 0; i < 200; ++i) {
    if (queue.push(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0})) {
      ++accepted;
    }
  }
  // Bounded, and it said no rather than growing or waiting.
  REQUIRE(accepted == 63);
}
