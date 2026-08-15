#pragma once

#include <cstdint>

namespace aidj {

/**
 * Three-band mixer-style EQ: low shelf, mid bell, high shelf.
 *
 * Shelving and peaking filters rather than a crossover network, for one
 * decisive reason: at neutral settings every coefficient collapses to a
 * pass-through, so a transition that is not using EQ is bit-exactly
 * unprocessed. A Linkwitz-Riley crossover bank has to reconstruct the signal
 * from three filtered copies, which leaves phase ripple around the crossover
 * points even when nothing is being cut - audible as a hollow quality on
 * material that is simply being played.
 *
 * This is also what hardware DJ mixers actually do, so the behaviour matches
 * what anyone used to one would expect.
 *
 * Band gains are linear 0..1, where 1 is neutral and 0 is a kill. They are
 * smoothed per sample: the transition engine moves them continuously over
 * seconds, and a stepped coefficient change is an audible click.
 */
class ThreeBandEq {
 public:
  void configure(int32_t sampleRate);

  /**
   * Targets for each band, 0..1. Reached over a short ramp rather than
   * immediately; see kSmoothingSeconds in the implementation.
   */
  void setBandGains(float low, float mid, float high);

  /** Jumps straight to the targets. For starting a voice, not for automation. */
  void snapToTargets();

  /** Processes one interleaved stereo buffer in place. */
  void process(float* interleaved, size_t frameCount);

  void reset();

  /** True when all three bands are neutral, so processing can be skipped. */
  bool isNeutral() const;

 private:
  struct Biquad {
    // Direct form I, per channel state.
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1[2] = {0.0, 0.0};
    double x2[2] = {0.0, 0.0};
    double y1[2] = {0.0, 0.0};
    double y2[2] = {0.0, 0.0};

    void reset();
    float process(float input, size_t channel);
  };

  void updateCoefficients();

  int32_t sampleRate_ = 48000;

  float targetLow_ = 1.0f;
  float targetMid_ = 1.0f;
  float targetHigh_ = 1.0f;

  float currentLow_ = 1.0f;
  float currentMid_ = 1.0f;
  float currentHigh_ = 1.0f;

  float smoothing_ = 0.0f;

  Biquad lowShelf_;
  Biquad midPeak_;
  Biquad highShelf_;
};

}  // namespace aidj
