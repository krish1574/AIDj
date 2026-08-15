#include "mix/TransitionTimeline.h"

#include <algorithm>
#include <cmath>

namespace aidj {
namespace {

constexpr double kPi = 3.14159265358979323846;

float lerp(float from, float to, float t) { return from + (to - from) * t; }

}  // namespace

void equalPowerGains(float progress, float& outgoing, float& incoming) {
  const float t = std::clamp(progress, 0.0f, 1.0f);
  const float angle = t * static_cast<float>(kPi) * 0.5f;
  outgoing = std::cos(angle);
  incoming = std::sin(angle);
}

void TransitionTimeline::arm(const TransitionSpec& spec, int64_t startFrame,
                             int32_t sampleRate) {
  spec_ = spec;
  startFrame_ = startFrame;

  const double frames =
      spec.durationMs * static_cast<double>(sampleRate) / 1000.0;
  // At least one frame long, so a degenerate plan cannot produce a transition
  // that starts and ends on the same frame and divides by zero below.
  endFrame_ = startFrame + std::max<int64_t>(1, static_cast<int64_t>(frames));
  armed_ = true;
}

void TransitionTimeline::clear() {
  armed_ = false;
  startFrame_ = 0;
  endFrame_ = 0;
}

float TransitionTimeline::progressAt(int64_t frame) const {
  if (!armed_) return 0.0f;
  if (frame <= startFrame_) return 0.0f;
  if (frame >= endFrame_) return 1.0f;
  return static_cast<float>(frame - startFrame_) /
         static_cast<float>(endFrame_ - startFrame_);
}

VoiceParameters TransitionTimeline::outgoingAt(int64_t frame) const {
  VoiceParameters parameters;
  if (!armed_) return parameters;

  parameters.tempoRatio = spec_.outgoingTempoRatio;

  if (frame < startFrame_) {
    // Not started: full level, EQ wherever the plan says it begins. Beginning
    // at anything other than neutral would be an audible step at the moment
    // the transition starts, so plans are expected to start from neutral.
    parameters.gain = spec_.outgoingGain;
    parameters.low = spec_.outgoingLowFrom;
    parameters.mid = spec_.outgoingMidFrom;
    return parameters;
  }

  if (frame >= endFrame_) {
    // Mixed out. Gain is zero rather than "very small": leaving a residue of
    // the previous track audible under the new one is exactly the muddiness
    // the bass swap exists to avoid.
    parameters.gain = 0.0f;
    parameters.low = spec_.outgoingLowTo;
    parameters.mid = spec_.outgoingMidTo;
    return parameters;
  }

  const float progress = progressAt(frame);

  float fadeOut = 1.0f;
  float fadeIn = 0.0f;
  equalPowerGains(progress, fadeOut, fadeIn);

  // A gapless plan does not overlap: the outgoing track simply plays to its
  // end, so no fade is applied.
  parameters.gain = spec_.overlap ? spec_.outgoingGain * fadeOut
                                  : spec_.outgoingGain;
  parameters.low = lerp(spec_.outgoingLowFrom, spec_.outgoingLowTo, progress);
  parameters.mid = lerp(spec_.outgoingMidFrom, spec_.outgoingMidTo, progress);
  return parameters;
}

VoiceParameters TransitionTimeline::incomingAt(int64_t frame) const {
  VoiceParameters parameters;
  if (!armed_) return parameters;

  parameters.tempoRatio = spec_.incomingTempoRatio;

  if (frame < startFrame_) {
    parameters.gain = 0.0f;
    parameters.low = spec_.incomingLowFrom;
    parameters.mid = spec_.incomingMidFrom;
    return parameters;
  }

  if (frame >= endFrame_) {
    parameters.gain = spec_.incomingGain;
    parameters.low = spec_.incomingLowTo;
    parameters.mid = spec_.incomingMidTo;
    return parameters;
  }

  const float progress = progressAt(frame);

  float fadeOut = 1.0f;
  float fadeIn = 0.0f;
  equalPowerGains(progress, fadeOut, fadeIn);

  parameters.gain = spec_.overlap ? spec_.incomingGain * fadeIn : 0.0f;
  parameters.low = lerp(spec_.incomingLowFrom, spec_.incomingLowTo, progress);
  parameters.mid = lerp(spec_.incomingMidFrom, spec_.incomingMidTo, progress);
  return parameters;
}

}  // namespace aidj
