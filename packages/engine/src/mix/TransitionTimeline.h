#pragma once

#include <cstdint>

namespace aidj {

/**
 * A transition, expressed in milliseconds, exactly as the TypeScript planner
 * produces it. Mirrors TransitionPlan in packages/core/src/transition.ts.
 *
 * Kept as a plain value type with no behaviour so it can cross JNI as a flat
 * array of numbers.
 */
struct TransitionSpec {
  double durationMs = 0.0;

  /** Where the incoming track starts playing from, milliseconds. */
  double incomingStartMs = 0.0;

  /** Loudness-matching gains, applied throughout and not part of the fade. */
  float outgoingGain = 1.0f;
  float incomingGain = 1.0f;

  /** Playback rate multipliers. 1.0 means untouched. */
  double outgoingTempoRatio = 1.0;
  double incomingTempoRatio = 1.0;

  /** EQ band multipliers at the start and end of the transition. */
  float outgoingLowFrom = 1.0f;
  float outgoingLowTo = 1.0f;
  float outgoingMidFrom = 1.0f;
  float outgoingMidTo = 1.0f;
  float incomingLowFrom = 1.0f;
  float incomingLowTo = 1.0f;
  float incomingMidFrom = 1.0f;
  float incomingMidTo = 1.0f;

  /** False for the gapless fallback, where no overlap happens at all. */
  bool overlap = true;
};

/** Everything one voice needs from the timeline at a given instant. */
struct VoiceParameters {
  float gain = 1.0f;
  float low = 1.0f;
  float mid = 1.0f;
  float high = 1.0f;
  double tempoRatio = 1.0;
};

/**
 * A transition compiled to absolute mixer frames.
 *
 * The audio thread queries this by frame number and gets back the exact
 * parameter values for that instant. It allocates nothing, locks nothing and
 * branches only on integer comparisons, because it is read from inside the
 * audio callback.
 *
 * Expressing the transition in absolute frames rather than "N buffers from
 * now" is what makes it sample-accurate: the result does not change if the
 * device hands us 96-frame callbacks one moment and 480-frame callbacks the
 * next, which Android does under load.
 */
class TransitionTimeline {
 public:
  /** Compiles `spec` to begin at `startFrame`. Safe to call from any thread. */
  void arm(const TransitionSpec& spec, int64_t startFrame, int32_t sampleRate);

  /** Discards any armed transition. */
  void clear();

  bool isArmed() const { return armed_; }
  int64_t startFrame() const { return startFrame_; }
  int64_t endFrame() const { return endFrame_; }

  /** True once the transition has run to completion at `frame`. */
  bool isComplete(int64_t frame) const {
    return armed_ && frame >= endFrame_;
  }

  /** True while the two voices overlap. */
  bool isActive(int64_t frame) const {
    return armed_ && frame >= startFrame_ && frame < endFrame_;
  }

  /**
   * Parameters for the outgoing voice at `frame`.
   * Before the transition: untouched but for loudness gain.
   * After it: silent, because the track has been mixed out.
   */
  VoiceParameters outgoingAt(int64_t frame) const;

  /**
   * Parameters for the incoming voice at `frame`.
   * Before the transition: silent, since it has not been introduced yet.
   */
  VoiceParameters incomingAt(int64_t frame) const;

  /** 0 at the start, 1 at the end. Clamped outside the transition. */
  float progressAt(int64_t frame) const;

 private:
  TransitionSpec spec_{};
  bool armed_ = false;
  int64_t startFrame_ = 0;
  int64_t endFrame_ = 0;
};

/**
 * Equal-power crossfade gains.
 *
 * Two uncorrelated signals sum in power, so a linear fade dips about 3 dB in
 * the middle - an audible sag exactly where both tracks should be strongest.
 * Matches crossfadeGains() in packages/core/src/transition.ts.
 */
void equalPowerGains(float progress, float& outgoing, float& incoming);

}  // namespace aidj
