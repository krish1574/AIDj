#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "mix/TransitionTimeline.h"

using aidj::TransitionSpec;
using aidj::TransitionTimeline;
using aidj::VoiceParameters;

namespace {

constexpr int32_t kSampleRate = 48000;

/** A beat-matched plan with the bass swap, as the planner would emit. */
TransitionSpec bassSwapSpec(double durationMs = 8000.0) {
  TransitionSpec spec;
  spec.durationMs = durationMs;
  spec.outgoingGain = 1.0f;
  spec.incomingGain = 1.0f;
  spec.outgoingLowFrom = 1.0f;
  spec.outgoingLowTo = 0.0f;
  spec.outgoingMidFrom = 1.0f;
  spec.outgoingMidTo = 0.6f;
  spec.incomingLowFrom = 0.0f;
  spec.incomingLowTo = 1.0f;
  spec.incomingMidFrom = 0.6f;
  spec.incomingMidTo = 1.0f;
  return spec;
}

}  // namespace

TEST_CASE("timeline is inert until armed", "[transition]") {
  TransitionTimeline timeline;
  REQUIRE_FALSE(timeline.isArmed());
  REQUIRE_FALSE(timeline.isActive(1000));
  // An unarmed timeline must leave a playing voice completely alone.
  REQUIRE(timeline.outgoingAt(1000).gain == 1.0f);
}

TEST_CASE("timeline starts and ends on exact frames", "[transition]") {
  // Sample accuracy is the point: the result must not depend on where a
  // callback boundary happens to fall, and Android varies the buffer size
  // under load.
  TransitionTimeline timeline;
  timeline.arm(bassSwapSpec(1000.0), 100000, kSampleRate);

  REQUIRE(timeline.startFrame() == 100000);
  REQUIRE(timeline.endFrame() == 100000 + kSampleRate);

  REQUIRE_FALSE(timeline.isActive(99999));
  REQUIRE(timeline.isActive(100000));
  REQUIRE(timeline.isActive(100000 + kSampleRate - 1));
  REQUIRE_FALSE(timeline.isActive(100000 + kSampleRate));
  REQUIRE(timeline.isComplete(100000 + kSampleRate));
}

TEST_CASE("crossfade holds constant power throughout", "[transition]") {
  TransitionTimeline timeline;
  const int64_t start = 48000;
  timeline.arm(bassSwapSpec(4000.0), start, kSampleRate);

  const int64_t end = timeline.endFrame();
  for (int64_t frame = start; frame < end; frame += 137) {
    const float outgoing = timeline.outgoingAt(frame).gain;
    const float incoming = timeline.incomingAt(frame).gain;
    const float power = outgoing * outgoing + incoming * incoming;
    INFO("frame " << frame << " power " << power);
    REQUIRE(std::abs(power - 1.0f) < 1e-4f);
  }
}

TEST_CASE("outgoing voice is silent after the transition", "[transition]") {
  // Leaving a residue of the previous track audible under the new one is
  // exactly the muddiness the bass swap exists to prevent.
  TransitionTimeline timeline;
  timeline.arm(bassSwapSpec(2000.0), 0, kSampleRate);
  REQUIRE(timeline.outgoingAt(timeline.endFrame()).gain == 0.0f);
  REQUIRE(timeline.outgoingAt(timeline.endFrame() + 100000).gain == 0.0f);
}

TEST_CASE("incoming voice is silent before the transition", "[transition]") {
  TransitionTimeline timeline;
  timeline.arm(bassSwapSpec(2000.0), 50000, kSampleRate);
  REQUIRE(timeline.incomingAt(0).gain == 0.0f);
  REQUIRE(timeline.incomingAt(49999).gain == 0.0f);
}

TEST_CASE("bass swaps across the transition", "[transition]") {
  TransitionTimeline timeline;
  const int64_t start = 0;
  timeline.arm(bassSwapSpec(4000.0), start, kSampleRate);
  const int64_t end = timeline.endFrame();
  const int64_t middle = end / 2;

  // Outgoing bass leaves, incoming bass arrives.
  REQUIRE(timeline.outgoingAt(start).low == 1.0f);
  REQUIRE(timeline.outgoingAt(end).low == 0.0f);
  REQUIRE(timeline.incomingAt(start).low == 0.0f);
  REQUIRE(timeline.incomingAt(end).low == 1.0f);

  // And they cross over rather than jumping.
  const float outgoingMid = timeline.outgoingAt(middle).low;
  const float incomingMid = timeline.incomingAt(middle).low;
  REQUIRE(outgoingMid > 0.3f);
  REQUIRE(outgoingMid < 0.7f);
  REQUIRE(incomingMid > 0.3f);
  REQUIRE(incomingMid < 0.7f);
}

TEST_CASE("parameters never jump between adjacent frames", "[transition]") {
  // Any discontinuity here is a click in the output. Checked per frame across
  // the whole transition, including both boundaries.
  TransitionTimeline timeline;
  timeline.arm(bassSwapSpec(1000.0), 1000, kSampleRate);

  VoiceParameters previousOut = timeline.outgoingAt(999);
  VoiceParameters previousIn = timeline.incomingAt(999);

  for (int64_t frame = 1000; frame <= timeline.endFrame(); ++frame) {
    const VoiceParameters out = timeline.outgoingAt(frame);
    const VoiceParameters in = timeline.incomingAt(frame);

    // One frame at 48 kHz over a 1 s fade moves any parameter by at most
    // ~2e-5; 1e-3 is a generous ceiling that still catches a real step.
    REQUIRE(std::abs(out.gain - previousOut.gain) < 1e-3f);
    REQUIRE(std::abs(out.low - previousOut.low) < 1e-3f);
    REQUIRE(std::abs(in.gain - previousIn.gain) < 1e-3f);
    REQUIRE(std::abs(in.low - previousIn.low) < 1e-3f);

    previousOut = out;
    previousIn = in;
  }
}

TEST_CASE("loudness gain scales the fade without breaking it", "[transition]") {
  TransitionSpec spec = bassSwapSpec(2000.0);
  spec.outgoingGain = 0.5f;   // a loud track pulled down
  spec.incomingGain = 1.6f;   // a quiet one lifted

  TransitionTimeline timeline;
  timeline.arm(spec, 0, kSampleRate);

  REQUIRE(timeline.outgoingAt(0).gain == 0.5f);
  REQUIRE(timeline.incomingAt(timeline.endFrame()).gain == 1.6f);

  // The fade shape still applies on top of the loudness correction.
  const float middleOut = timeline.outgoingAt(timeline.endFrame() / 2).gain;
  REQUIRE(middleOut < 0.5f);
  REQUIRE(middleOut > 0.0f);
}

TEST_CASE("tempo ratio is reported for both voices at all times", "[transition]") {
  // The stretcher needs a ratio before the transition begins, not only during
  // it: the incoming track has to already be at the right tempo when it is
  // introduced, or the first bars drift.
  TransitionSpec spec = bassSwapSpec();
  spec.incomingTempoRatio = 1.031;

  TransitionTimeline timeline;
  timeline.arm(spec, 10000, kSampleRate);

  REQUIRE(timeline.incomingAt(0).tempoRatio == 1.031);
  REQUIRE(timeline.incomingAt(15000).tempoRatio == 1.031);
  REQUIRE(timeline.incomingAt(timeline.endFrame() + 1).tempoRatio == 1.031);
}

TEST_CASE("gapless plans do not overlap the two tracks", "[transition]") {
  TransitionSpec spec = bassSwapSpec(2000.0);
  spec.overlap = false;

  TransitionTimeline timeline;
  timeline.arm(spec, 0, kSampleRate);

  // The outgoing track plays out at full level and the incoming one stays
  // silent until it is over - which is the point of the fallback.
  REQUIRE(timeline.outgoingAt(timeline.endFrame() / 2).gain == 1.0f);
  REQUIRE(timeline.incomingAt(timeline.endFrame() / 2).gain == 0.0f);
}

TEST_CASE("a zero-length plan cannot divide by zero", "[transition]") {
  TransitionTimeline timeline;
  timeline.arm(bassSwapSpec(0.0), 0, kSampleRate);
  REQUIRE(timeline.endFrame() > timeline.startFrame());
  REQUIRE(std::isfinite(timeline.progressAt(0)));
  REQUIRE(std::isfinite(timeline.outgoingAt(0).gain));
}
