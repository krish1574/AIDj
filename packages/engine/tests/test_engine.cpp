#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include "Engine.h"
#include "support/NullOutput.h"
#include "support/TestDecoder.h"

using aidj::Engine;
using aidj::EngineError;
using aidj::EngineState;
using aidj::test::NullOutput;

namespace {

/** 2 seconds of 440 Hz stereo at 44.1 kHz - exercises the resampler too. */
constexpr const char* kToneA = "test://sine?hz=440&rate=44100&ch=2&ms=2000";
constexpr const char* kToneB = "test://sine?hz=880&rate=48000&ch=2&ms=2000";

float peak(const std::vector<float>& samples, size_t from, size_t to) {
  float result = 0.0f;
  for (size_t i = from; i < to && i < samples.size(); ++i) {
    result = std::max(result, std::fabs(samples[i]));
  }
  return result;
}

}  // namespace

TEST_CASE("Engine starts uninitialised and rejects work until initialised") {
  Engine engine;
  REQUIRE(engine.status().state == EngineState::Uninitialised);
  REQUIRE(engine.loadVoice(0, kToneA) == EngineError::InvalidState);
}

TEST_CASE("Engine reaches Idle after the output opens") {
  Engine engine;
  REQUIRE(engine.initialise(std::make_unique<NullOutput>()) ==
          EngineError::None);
  REQUIRE(engine.status().state == EngineState::Idle);
}

TEST_CASE("Engine reports a missing file rather than throwing") {
  Engine engine;
  engine.initialise(std::make_unique<NullOutput>());
  REQUIRE(engine.loadVoice(0, "file:///does/not/exist.mp3") ==
          EngineError::FileNotFound);
  REQUIRE_FALSE(engine.status().voices[0].hasSource);
}

TEST_CASE("Engine rejects an unsupported source") {
  Engine engine;
  engine.initialise(std::make_unique<NullOutput>());
  REQUIRE(engine.loadVoice(0, "test://unsupported") ==
          EngineError::DecoderUnsupportedFormat);
}

TEST_CASE("Engine rejects an out-of-range voice index") {
  Engine engine;
  engine.initialise(std::make_unique<NullOutput>());
  REQUIRE(engine.loadVoice(7, kToneA) == EngineError::InvalidState);
  REQUIRE(engine.loadVoice(-1, kToneA) == EngineError::InvalidState);
}

TEST_CASE("Engine loads a voice, primes it, and produces real audio") {
  auto output = std::make_unique<NullOutput>();
  NullOutput* outputPtr = output.get();

  Engine engine;
  REQUIRE(engine.initialise(std::move(output)) == EngineError::None);
  REQUIRE(engine.loadVoice(0, kToneA) == EngineError::None);
  REQUIRE(engine.status().state == EngineState::Preparing);

  // Wait for the decoder thread to prime rather than sleeping a fixed time.
  for (int i = 0; i < 500 && !engine.status().voices[0].primed; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE(engine.status().voices[0].primed);

  REQUIRE(engine.playVoice(0) == EngineError::None);
  REQUIRE(engine.status().state == EngineState::Playing);

  outputPtr->waitForFrames(24000);
  const auto captured = outputPtr->captured();
  engine.shutdown();

  // Skip the 20 ms start ramp, then require actual signal - not silence.
  REQUIRE(peak(captured, 4000, captured.size()) > 0.2f);
}

TEST_CASE("Engine never exceeds the output ceiling with both voices loud") {
  auto output = std::make_unique<NullOutput>();
  NullOutput* outputPtr = output.get();

  Engine engine;
  engine.initialise(std::move(output));
  REQUIRE(engine.loadVoice(0, kToneA) == EngineError::None);
  REQUIRE(engine.loadVoice(1, kToneB) == EngineError::None);

  for (int i = 0; i < 500; ++i) {
    const auto status = engine.status();
    if (status.voices[0].primed && status.voices[1].primed) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  engine.playVoice(0);
  engine.playVoice(1);
  outputPtr->waitForFrames(48000);

  const auto captured = outputPtr->captured();
  engine.shutdown();

  REQUIRE(peak(captured, 0, captured.size()) <= aidj::kOutputCeiling);
}

TEST_CASE("Dev crossfade is rejected unless both voices are loaded") {
  Engine engine;
  engine.initialise(std::make_unique<NullOutput>());
  REQUIRE(engine.devCrossfade(0, 1, 2000) == EngineError::InvalidState);

  REQUIRE(engine.loadVoice(0, kToneA) == EngineError::None);
  REQUIRE(engine.devCrossfade(0, 1, 2000) == EngineError::InvalidState);

  REQUIRE(engine.loadVoice(1, kToneB) == EngineError::None);
  REQUIRE(engine.devCrossfade(0, 1, 2000) == EngineError::None);
}

TEST_CASE("Dev crossfade rejects nonsense durations and self-transitions") {
  Engine engine;
  engine.initialise(std::make_unique<NullOutput>());
  engine.loadVoice(0, kToneA);
  engine.loadVoice(1, kToneB);

  REQUIRE(engine.devCrossfade(0, 0, 2000) == EngineError::InvalidState);
  REQUIRE(engine.devCrossfade(0, 1, 10) == EngineError::InvalidState);
  REQUIRE(engine.devCrossfade(0, 1, 120000) == EngineError::InvalidState);
}

TEST_CASE("Pause and resume follow the state machine, not booleans") {
  Engine engine;
  engine.initialise(std::make_unique<NullOutput>());

  // Cannot pause what is not playing.
  REQUIRE(engine.pause() == EngineError::InvalidState);

  engine.loadVoice(0, kToneA);
  engine.playVoice(0);
  REQUIRE(engine.status().state == EngineState::Playing);

  REQUIRE(engine.pause() == EngineError::None);
  REQUIRE(engine.status().state == EngineState::Paused);

  // Cannot pause twice.
  REQUIRE(engine.pause() == EngineError::InvalidState);

  REQUIRE(engine.resume() == EngineError::None);
  REQUIRE(engine.status().state == EngineState::Playing);
  REQUIRE(engine.resume() == EngineError::InvalidState);
}

TEST_CASE("Engine closes every decoder it opened") {
  {
    Engine engine;
    engine.initialise(std::make_unique<NullOutput>());
    engine.loadVoice(0, kToneA);
    engine.loadVoice(1, kToneB);
    engine.loadVoice(0, kToneB);  // replace voice 0's source
  }
  REQUIRE(aidj::test::TestDecoder::openCount().load() == 0);
}
