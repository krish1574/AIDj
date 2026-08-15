#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "AudioTypes.h"
#include "DecodePump.h"
#include "IAudioOutput.h"
#include "mix/Mixer.h"

namespace aidj {

struct VoiceStatus {
  bool hasSource = false;
  int64_t positionMs = 0;
  int64_t durationMs = 0;
  float gain = 0.0f;
  bool primed = false;
  bool endOfStream = false;
};

struct EngineStatus {
  EngineState state = EngineState::Uninitialised;
  std::array<VoiceStatus, kVoiceCount> voices{};
  int32_t underrunCount = 0;
  int32_t framesPerBurst = 0;
  double outputLatencyMs = -1.0;
  int64_t starvedFrames = 0;
  EngineError lastError = EngineError::None;
};

/**
 * The authoritative playback state machine.
 *
 * It lives here, in C++, next to the sample clock - not in JavaScript. The JS
 * layer holds a read-only mirror fed by polling this status. Keeping a second
 * mutable copy of "are we playing" on the JS thread would race the audio
 * thread, and that race is exactly the bug class that produces gaps.
 *
 * Public methods are called from the JS/JNI thread and are serialised by
 * `controlMutex_`. They communicate with the audio thread only through the
 * mixer's lock-free command queue and through atomics.
 */
class Engine {
 public:
  Engine();
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /** Opens the output stream. Idempotent. */
  EngineError initialise(std::unique_ptr<IAudioOutput> output);
  void shutdown();

  /**
   * Decodes `uri` into `voiceIndex`, replacing whatever was there. Returns once
   * the source is open and the decoder thread is running - not once it is full.
   * Poll `status().voices[i].primed` for readiness.
   */
  EngineError loadVoice(int32_t voiceIndex, const std::string& uri);

  /** Makes a loaded voice audible at unity gain and starts the transport. */
  EngineError playVoice(int32_t voiceIndex);

  EngineError pause();
  EngineError resume();
  void stopAll();

  /** Milestone 1 developer tool only. See DevCrossfadeRequest in core. */
  EngineError devCrossfade(int32_t fromVoice, int32_t toVoice,
                           int32_t durationMs);

  /**
   * Loads a voice and cues it to `startMs` at a given playback rate.
   *
   * This is what "prepare the next track" means in practice: the incoming
   * track has to be decoding, already at the matched tempo, and positioned at
   * its cue point before the transition begins. Doing any of that at the
   * moment of the transition would produce exactly the gap the product exists
   * to avoid.
   */
  EngineError prepareVoice(int32_t voiceIndex, const std::string& uri,
                           double startMs, double tempoRatio);

  /**
   * Arms a planned transition between two prepared voices.
   *
   * `delayMs` is how far ahead of now it should begin, which is how a
   * transition is made to land on a chosen beat rather than whenever the
   * command happens to be processed.
   */
  EngineError armTransition(const TransitionSpec& spec, int32_t outgoingVoice,
                            int32_t incomingVoice, double delayMs);

  /** Cancels an armed transition, leaving both voices where they are. */
  void clearTransition();

  /** Increments each time a transition completes. Polled by the UI. */
  int64_t transitionsCompleted() const { return mixer_.transitionsCompleted(); }

  EngineStatus status() const;

 private:
  bool isValidVoice(int32_t index) const {
    return index >= 0 && index < kVoiceCount;
  }
  void setState(EngineState state) {
    state_.store(state, std::memory_order_release);
  }

  mutable std::mutex controlMutex_;
  Mixer mixer_;
  std::unique_ptr<IAudioOutput> output_;
  std::array<std::unique_ptr<DecodePump>, kVoiceCount> pumps_{};
  std::array<bool, kVoiceCount> voiceLoaded_{};
  std::atomic<EngineState> state_{EngineState::Uninitialised};
  std::atomic<EngineError> lastError_{EngineError::None};
};

}  // namespace aidj
