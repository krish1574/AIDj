#include "analysis/Loudness.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace aidj {
namespace {

constexpr double kPi = 3.14159265358979323846;
/** BS.1770 offset relating mean square to LKFS. */
constexpr double kLoudnessOffset = -0.691;
constexpr double kAbsoluteGateLufs = -70.0;
constexpr double kRelativeGateLu = -10.0;

}  // namespace

LoudnessMeter::LoudnessMeter(int32_t sampleRate) {
  const double fs = static_cast<double>(sampleRate);

  // Stage 1: high-frequency shelving filter, +4 dB above ~1.5 kHz. Derived
  // from the standard's reference coefficients rather than hardcoded for
  // 48 kHz, so analysis is correct at whatever rate the decoder hands us.
  {
    const double f0 = 1681.974450955533;
    const double gainDb = 3.999843853973347;
    const double q = 0.7071752369554196;

    const double k = std::tan(kPi * f0 / fs);
    const double vh = std::pow(10.0, gainDb / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double denominator = 1.0 + k / q + k * k;

    shelfB_[0] = (vh + vb * k / q + k * k) / denominator;
    shelfB_[1] = 2.0 * (k * k - vh) / denominator;
    shelfB_[2] = (vh - vb * k / q + k * k) / denominator;
    shelfA_[0] = 2.0 * (k * k - 1.0) / denominator;
    shelfA_[1] = (1.0 - k / q + k * k) / denominator;
  }

  // Stage 2: high-pass at ~38 Hz, modelling insensitivity to deep bass.
  {
    const double f0 = 38.13547087602444;
    const double q = 0.5003270373238773;
    const double k = std::tan(kPi * f0 / fs);

    highB_[0] = 1.0;
    highB_[1] = -2.0;
    highB_[2] = 1.0;
    highA_[0] = 2.0 * (k * k - 1.0) / (1.0 + k / q + k * k);
    highA_[1] = (1.0 - k / q + k * k) / (1.0 + k / q + k * k);
  }

  blockSize_ = static_cast<size_t>(fs * 0.4);
  hopSize_ = static_cast<size_t>(fs * 0.1);
  accumulator_.assign(blockSize_, 0.0);
}

void LoudnessMeter::process(const float* samples, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const double input = static_cast<double>(samples[i]);
    peak_ = std::max(peak_, std::abs(input));

    // Cascade the two K-weighting biquads (direct form I).
    const double shelfOut = shelfB_[0] * input + shelfB_[1] * shelfX1_ +
                            shelfB_[2] * shelfX2_ - shelfA_[0] * shelfY1_ -
                            shelfA_[1] * shelfY2_;
    shelfX2_ = shelfX1_;
    shelfX1_ = input;
    shelfY2_ = shelfY1_;
    shelfY1_ = shelfOut;

    const double highOut = highB_[0] * shelfOut + highB_[1] * highX1_ +
                           highB_[2] * highX2_ - highA_[0] * highY1_ -
                           highA_[1] * highY2_;
    highX2_ = highX1_;
    highX1_ = shelfOut;
    highY2_ = highY1_;
    highY1_ = highOut;

    accumulator_[accumulatorFill_] = highOut * highOut;
    accumulatorFill_ += 1;

    if (accumulatorFill_ == blockSize_) {
      const double sum =
          std::accumulate(accumulator_.begin(), accumulator_.end(), 0.0);
      pushBlockPower(sum / static_cast<double>(blockSize_));

      // Slide by the hop, keeping 75% overlap as the standard requires.
      std::move(accumulator_.begin() + static_cast<long>(hopSize_),
                accumulator_.end(), accumulator_.begin());
      accumulatorFill_ = blockSize_ - hopSize_;
    }
  }
}

void LoudnessMeter::pushBlockPower(double power) {
  blockPowers_.push_back(power);
}

LoudnessResult LoudnessMeter::finish() {
  LoudnessResult result;
  result.peakDbfs = peak_ > 1e-12 ? 20.0 * std::log10(peak_) : -144.0;

  if (blockPowers_.empty()) {
    result.integratedLufs = kAbsoluteGateLufs;
    return result;
  }

  const auto loudnessOf = [](double power) {
    return power > 1e-15 ? kLoudnessOffset + 10.0 * std::log10(power)
                         : -200.0;
  };

  // Absolute gate: discard near-silence so leading and trailing digital
  // silence cannot drag the average down.
  std::vector<double> gated;
  gated.reserve(blockPowers_.size());
  for (double power : blockPowers_) {
    if (loudnessOf(power) > kAbsoluteGateLufs) gated.push_back(power);
  }

  if (gated.empty()) {
    result.integratedLufs = kAbsoluteGateLufs;
    return result;
  }

  // Relative gate: discard blocks more than 10 LU below the ungated mean.
  // This is what stops quiet passages from dominating the measurement.
  const double ungatedMean =
      std::accumulate(gated.begin(), gated.end(), 0.0) /
      static_cast<double>(gated.size());
  const double threshold = loudnessOf(ungatedMean) + kRelativeGateLu;

  double sum = 0.0;
  size_t count = 0;
  for (double power : gated) {
    if (loudnessOf(power) > threshold) {
      sum += power;
      count += 1;
    }
  }

  result.integratedLufs =
      count > 0 ? loudnessOf(sum / static_cast<double>(count))
                : loudnessOf(ungatedMean);
  return result;
}

}  // namespace aidj
