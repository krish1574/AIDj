#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "../AudioTypes.h"
#include "../IAudioOutput.h"
#include "../RingBuffer.h"

namespace aidj {

/**
 * A single playback voice as seen by the audio thread.
 *
 * The decoder thread owns `ring` (producer side) and `endOfStream`. The audio
 * thread owns everything else. Nothing here is mutated from the JS thread -
 * the JS thread may only post commands, which the audio thread drains.
 */
struct Voice {
  explicit Voice(size_t ringCapacitySamples) : ring(ringCapacitySamples) {}

  RingBuffer ring;
  /** Set by the decoder thread once the source has been fully decoded. */
  std::atomic<bool> endOfStream{false};
  /** Set once the ring holds enough to start without immediately starving. */
  std::atomic<bool> primed{false};
  /** Frames handed to the mixer so far - the voice's playhead. */
  std::atomic<int64_t> framesRendered{0};
  std::atomic<int64_t> durationFrames{0};
  /** Observable copy of the applied gain, for the UI. */
  std::atomic<float> currentGain{0.0f};
  /** False means the mixer skips this voice entirely. */
  std::atomic<bool> active{false};
};

enum class CommandType : int32_t {
  None = 0,
  SetVoiceActive,
  SetVoiceGain,
  DevCrossfade,
  Reset,
};

struct Command {
  CommandType type = CommandType::None;
  int32_t voice = 0;
  int32_t voiceB = 0;
  float value = 0.0f;
  int32_t durationFrames = 0;
};

/**
 * Fixed-capacity SPSC command queue. Producer is the JS/JNI thread, consumer is
 * the audio callback. Posting from JS never blocks the audio thread and the
 * audio thread never allocates to receive a command.
 */
class CommandQueue {
 public:
  bool push(const Command& command);
  bool pop(Command& out);

 private:
  static constexpr size_t kCapacity = 64;  // power of two
  static constexpr size_t kMask = kCapacity - 1;
  std::array<Command, kCapacity> slots_{};
  std::atomic<size_t> writeIndex_{0};
  std::atomic<size_t> readIndex_{0};
};

/**
 * Per-voice gain automation.
 *
 * A ramp is expressed in absolute mixer frames so it is sample-accurate and
 * independent of buffer size. This is the mechanism the real transition engine
 * (Milestone 5) will drive; Milestone 1 only ever populates it from the
 * developer crossfade command.
 */
struct GainRamp {
  int64_t startFrame = 0;
  int64_t endFrame = 0;
  float startGain = 0.0f;
  float endGain = 0.0f;
  /** Equal-power when true, linear when false. */
  bool equalPower = false;

  float valueAt(int64_t frame) const;
};

class Mixer final : public IAudioRenderer {
 public:
  Mixer();

  void render(float* output, int32_t frameCount) override;

  /** Callable from any non-audio thread. */
  bool post(const Command& command);

  Voice& voice(int32_t index) { return *voices_[static_cast<size_t>(index)]; }
  const Voice& voice(int32_t index) const {
    return *voices_[static_cast<size_t>(index)];
  }

  int64_t frameClock() const { return frameClock_.load(std::memory_order_relaxed); }
  /** Frames the mixer wanted but the decoders had not produced. */
  int64_t starvedFrames() const {
    return starvedFrames_.load(std::memory_order_relaxed);
  }

 private:
  void drainCommands();
  static float softLimit(float sample);

  std::array<std::unique_ptr<Voice>, kVoiceCount> voices_;
  std::array<GainRamp, kVoiceCount> ramps_{};
  CommandQueue commands_;
  std::atomic<int64_t> frameClock_{0};
  std::atomic<int64_t> starvedFrames_{0};
  /** Pre-allocated scratch, sized for a generous callback. Never resized. */
  std::vector<float> scratch_;
};

}  // namespace aidj
