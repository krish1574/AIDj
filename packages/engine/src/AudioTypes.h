#pragma once

#include <cstdint>

namespace aidj {

/**
 * The engine runs at a fixed internal format. Everything is resampled to this
 * on decode so the mixer never has to reason about rate mismatches between two
 * voices mid-transition.
 */
constexpr int32_t kEngineSampleRate = 48000;
constexpr int32_t kEngineChannelCount = 2;
constexpr int32_t kVoiceCount = 2;

/** True-peak ceiling for the output limiter, linear. -1 dBFS. */
constexpr float kOutputCeiling = 0.891251f;

enum class EngineState : int32_t {
  Uninitialised = 0,
  Idle = 1,
  Preparing = 2,
  Playing = 3,
  Paused = 4,
  Error = 5,
};

enum class EngineError : int32_t {
  None = 0,
  OutputOpenFailed = 1,
  DecoderUnsupportedFormat = 2,
  DecoderIoError = 3,
  FileNotFound = 4,
  VoiceBusy = 5,
  InvalidState = 6,
};

const char* toString(EngineError error);

}  // namespace aidj
