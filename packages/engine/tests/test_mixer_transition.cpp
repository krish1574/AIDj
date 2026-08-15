#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "mix/Mixer.h"

using aidj::Command;
using aidj::CommandType;
using aidj::kEngineChannelCount;
using aidj::kEngineSampleRate;
using aidj::Mixer;
using aidj::TransitionSpec;

namespace {

constexpr int32_t kBlockFrames = 256;

/** Fills a voice's ring with a constant so gain changes are easy to read. */
void fillVoice(Mixer& mixer, int32_t voiceIndex, float value, size_t frames) {
  std::vector<float> block(frames * kEngineChannelCount, value);
  mixer.voice(voiceIndex).ring.write(block.data(), block.size());
  mixer.voice(voiceIndex).primed.store(true, std::memory_order_relaxed);
}

TransitionSpec plainCrossfade(double durationMs) {
  TransitionSpec spec;
  spec.durationMs = durationMs;
  spec.outgoingGain = 1.0f;
  spec.incomingGain = 1.0f;
  // EQ left neutral so the test measures the fade, not the filters.
  return spec;
}

/** Renders `blocks` callbacks and returns the interleaved output. */
std::vector<float> renderBlocks(Mixer& mixer, int blocks) {
  std::vector<float> all;
  std::vector<float> buffer(static_cast<size_t>(kBlockFrames) *
                            kEngineChannelCount);
  for (int i = 0; i < blocks; ++i) {
    mixer.render(buffer.data(), kBlockFrames);
    all.insert(all.end(), buffer.begin(), buffer.end());
  }
  return all;
}

}  // namespace

TEST_CASE("armed transition crossfades between two voices", "[mixer]") {
  Mixer mixer;

  const size_t frames = static_cast<size_t>(kEngineSampleRate);
  fillVoice(mixer, 0, 0.5f, frames);
  fillVoice(mixer, 1, 0.5f, frames);

  mixer.post(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0});
  mixer.post(Command{CommandType::SetVoiceGain, 0, 0, 1.0f, 0});

  // A 500 ms transition starting immediately.
  mixer.stageTransition(plainCrossfade(500.0), 0, 1);
  mixer.post(Command{CommandType::ArmTransition, 0, 0, 0.0f, 0});

  const int blocks =
      static_cast<int>((kEngineSampleRate / 2) / kBlockFrames) + 4;
  const std::vector<float> output = renderBlocks(mixer, blocks);

  // Both sources are the same constant, so an equal-power crossfade between
  // them should hold a roughly constant level throughout rather than dipping.
  const size_t totalFrames = output.size() / kEngineChannelCount;
  for (size_t f = 32; f + 32 < totalFrames; f += 64) {
    const float sample = std::abs(output[f * kEngineChannelCount]);
    INFO("frame " << f << " level " << sample);
    // cos+sin peaks at sqrt(2) mid-fade; the limiter shapes the top a little.
    REQUIRE(sample > 0.4f);
    REQUIRE(sample < 0.8f);
  }
}

TEST_CASE("transition retires itself and frees the outgoing voice", "[mixer]") {
  // A voice left active at zero gain still drains its ring and holds a
  // decoder, and the preparation pipeline needs that slot back.
  Mixer mixer;

  const size_t frames = static_cast<size_t>(kEngineSampleRate);
  fillVoice(mixer, 0, 0.5f, frames);
  fillVoice(mixer, 1, 0.5f, frames);

  mixer.post(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0});
  mixer.stageTransition(plainCrossfade(200.0), 0, 1);
  mixer.post(Command{CommandType::ArmTransition, 0, 0, 0.0f, 0});

  REQUIRE(mixer.transitionsCompleted() == 0);

  renderBlocks(mixer, static_cast<int>((kEngineSampleRate / 5) / kBlockFrames) + 8);

  REQUIRE(mixer.transitionsCompleted() == 1);
  REQUIRE_FALSE(mixer.voice(0).active.load());
  REQUIRE(mixer.voice(1).active.load());
  REQUIRE_FALSE(mixer.timeline().isArmed());
}

TEST_CASE("incoming voice keeps playing after the transition", "[mixer]") {
  // The whole point of a DJ set: when the fade ends the new track is simply
  // playing, at its loudness-matched level, with nothing left automating it.
  Mixer mixer;

  const size_t frames = static_cast<size_t>(kEngineSampleRate) * 2;
  fillVoice(mixer, 0, 0.5f, frames);
  fillVoice(mixer, 1, 0.5f, frames);

  mixer.post(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0});
  mixer.stageTransition(plainCrossfade(200.0), 0, 1);
  mixer.post(Command{CommandType::ArmTransition, 0, 0, 0.0f, 0});

  renderBlocks(mixer, static_cast<int>((kEngineSampleRate / 5) / kBlockFrames) + 8);
  const std::vector<float> after = renderBlocks(mixer, 20);

  bool heardSomething = false;
  for (size_t i = 0; i < after.size(); i += 64) {
    if (std::abs(after[i]) > 0.1f) heardSomething = true;
  }
  REQUIRE(heardSomething);
}

TEST_CASE("a delayed transition does not start early", "[mixer]") {
  // Arming ahead of time is how a transition lands on a chosen beat. It must
  // do nothing at all until that frame arrives.
  Mixer mixer;

  const size_t frames = static_cast<size_t>(kEngineSampleRate);
  fillVoice(mixer, 0, 0.5f, frames);
  fillVoice(mixer, 1, 0.5f, frames);

  mixer.post(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0});
  mixer.post(Command{CommandType::SetVoiceGain, 0, 0, 1.0f, 0});

  const int32_t delayFrames = kEngineSampleRate / 4;  // 250 ms
  mixer.stageTransition(plainCrossfade(200.0), 0, 1);
  mixer.post(Command{CommandType::ArmTransition, 0, 0, 0.0f, delayFrames});

  // Commands are consumed by the audio thread, so nothing is armed until a
  // callback has run. Render one block to let it take effect.
  renderBlocks(mixer, 1);

  REQUIRE(mixer.timeline().isArmed());
  REQUIRE(mixer.timeline().startFrame() >= delayFrames);
  REQUIRE_FALSE(mixer.timeline().isActive(0));

  // Render less than the delay; nothing should have completed.
  renderBlocks(mixer, static_cast<int>((delayFrames / 2) / kBlockFrames));
  REQUIRE(mixer.transitionsCompleted() == 0);
}

TEST_CASE("incoming voice is not consumed before the transition", "[mixer]") {
  // The bug this guards against was audible rather than theoretical: the
  // incoming voice is activated when the transition is armed so its decoder
  // can run, and it was also being read at zero gain during the wait. The
  // track silently played through itself, so the fade began seconds past the
  // downbeat the planner picked and the mix landed on the wrong bar.
  Mixer mixer;

  const size_t frames = static_cast<size_t>(kEngineSampleRate);
  fillVoice(mixer, 0, 0.5f, frames);
  fillVoice(mixer, 1, 0.5f, frames);

  mixer.post(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0});
  mixer.post(Command{CommandType::SetVoiceGain, 0, 0, 1.0f, 0});

  const size_t availableBefore = mixer.voice(1).ring.sizeAvailableToRead();

  // Arm half a second ahead and render a quarter of a second of it.
  mixer.stageTransition(plainCrossfade(200.0), 0, 1);
  mixer.post(Command{CommandType::ArmTransition, 0, 0, 0.0f,
                     kEngineSampleRate / 2});
  renderBlocks(mixer, static_cast<int>((kEngineSampleRate / 4) / kBlockFrames));

  // Not a single frame of the incoming track may have been consumed.
  REQUIRE(mixer.voice(1).ring.sizeAvailableToRead() == availableBefore);
  REQUIRE(mixer.voice(1).framesRendered.load() == 0);
}

TEST_CASE("clearing a transition restores ordinary playback", "[mixer]") {
  Mixer mixer;
  fillVoice(mixer, 0, 0.5f, static_cast<size_t>(kEngineSampleRate));
  fillVoice(mixer, 1, 0.5f, static_cast<size_t>(kEngineSampleRate));

  mixer.post(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0});
  mixer.stageTransition(plainCrossfade(5000.0), 0, 1);
  mixer.post(Command{CommandType::ArmTransition, 0, 0, 0.0f, 0});
  renderBlocks(mixer, 4);
  REQUIRE(mixer.timeline().isArmed());

  mixer.post(Command{CommandType::ClearTransition, 0, 0, 0.0f, 0});
  renderBlocks(mixer, 2);

  REQUIRE_FALSE(mixer.timeline().isArmed());
  REQUIRE(mixer.outgoingVoice() == -1);
}

TEST_CASE("output never exceeds the ceiling during a transition", "[mixer]") {
  // Two loud tracks crossfading is the moment most likely to clip.
  Mixer mixer;

  const size_t frames = static_cast<size_t>(kEngineSampleRate);
  fillVoice(mixer, 0, 0.95f, frames);
  fillVoice(mixer, 1, 0.95f, frames);

  mixer.post(Command{CommandType::SetVoiceActive, 0, 0, 1.0f, 0});
  mixer.stageTransition(plainCrossfade(300.0), 0, 1);
  mixer.post(Command{CommandType::ArmTransition, 0, 0, 0.0f, 0});

  const std::vector<float> output = renderBlocks(mixer, 80);
  for (float sample : output) {
    REQUIRE(std::isfinite(sample));
    REQUIRE(std::abs(sample) <= aidj::kOutputCeiling + 1e-4f);
  }
}
