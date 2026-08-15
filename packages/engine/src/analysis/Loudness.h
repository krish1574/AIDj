#pragma once

#include <cstddef>
#include <vector>

#include "analysis/AnalysisTypes.h"

namespace aidj {

/**
 * ITU-R BS.1770-4 integrated loudness.
 *
 * Implements the actual standard - K-weighting (a shelving filter plus a
 * high-pass, modelling head acoustics and perceived low-frequency
 * insensitivity), 400 ms blocks at 75% overlap, then the two-stage gate:
 * an absolute gate at -70 LUFS and a relative gate 10 LU below the ungated
 * mean.
 *
 * The gating is the part that matters for a DJ app. A plain RMS average rates
 * a track with long quiet passages as much quieter than it sounds, so
 * normalising by it would make the loud sections blast. The relative gate is
 * what makes two tracks matched by this measurement actually sound equally
 * loud.
 *
 * Mono-summed input is accepted, which is a documented simplification: the
 * standard specifies per-channel weighting with a +1.5 dB surround allowance.
 * For stereo music the difference is small and consistent, and analysis
 * already works on a mono mixdown.
 */
class LoudnessMeter {
 public:
  explicit LoudnessMeter(int32_t sampleRate);

  /** Feeds mono samples. Call repeatedly while streaming. */
  void process(const float* samples, size_t count);

  /** Finalises gating and returns the result. */
  LoudnessResult finish();

 private:
  void pushBlockPower(double power);

  // K-weighting biquad state (stage 1: high shelf, stage 2: high-pass).
  double shelfB_[3] = {0.0, 0.0, 0.0};
  double shelfA_[2] = {0.0, 0.0};
  double highB_[3] = {0.0, 0.0, 0.0};
  double highA_[2] = {0.0, 0.0};

  double shelfX1_ = 0.0, shelfX2_ = 0.0, shelfY1_ = 0.0, shelfY2_ = 0.0;
  double highX1_ = 0.0, highX2_ = 0.0, highY1_ = 0.0, highY2_ = 0.0;

  size_t blockSize_ = 0;   // 400 ms
  size_t hopSize_ = 0;     // 100 ms, giving 75% overlap
  std::vector<double> accumulator_;
  size_t accumulatorFill_ = 0;

  std::vector<double> blockPowers_;
  double peak_ = 0.0;
};

}  // namespace aidj
