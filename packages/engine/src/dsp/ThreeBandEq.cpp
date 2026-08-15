#include "dsp/ThreeBandEq.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aidj {
namespace {

constexpr double kPi = 3.14159265358979323846;

/** Band centres, chosen to match the ranges a DJ mixer's controls cover. */
constexpr double kLowShelfHz = 200.0;
constexpr double kMidPeakHz = 1000.0;
constexpr double kHighShelfHz = 4000.0;

/** Q for the mid bell. Wide enough to move a vocal range, not surgical. */
constexpr double kMidQ = 0.7;
/** Shelf slope. 0.7 is a gentle, natural-sounding shelf. */
constexpr double kShelfSlope = 0.7;

/**
 * Attenuation a band gain of 0 corresponds to.
 *
 * A true kill would need infinite attenuation, which no shelf provides. -40 dB
 * removes the band as far as anyone can hear while keeping the filter stable
 * and the coefficients well conditioned.
 */
constexpr double kKillDb = -40.0;

/** How long a band gain takes to reach a new target. */
constexpr float kSmoothingSeconds = 0.02f;

/** Linear 0..1 band gain to decibels. 1 -> 0 dB, 0 -> kKillDb. */
double bandGainToDecibels(float gain) {
  const double clamped = std::clamp(static_cast<double>(gain), 0.0, 1.0);
  if (clamped >= 0.999) return 0.0;
  // Map through amplitude so the control feels proportional rather than
  // collapsing at the top of its range.
  const double floorAmplitude = std::pow(10.0, kKillDb / 20.0);
  const double amplitude = floorAmplitude + (1.0 - floorAmplitude) * clamped;
  return 20.0 * std::log10(std::max(amplitude, 1e-6));
}

bool isNeutralGain(float gain) { return gain >= 0.999f; }

}  // namespace

void ThreeBandEq::Biquad::reset() {
  x1[0] = x1[1] = x2[0] = x2[1] = 0.0;
  y1[0] = y1[1] = y2[0] = y2[1] = 0.0;
}

float ThreeBandEq::Biquad::process(float input, size_t channel) {
  const double x = static_cast<double>(input);
  const double y = b0 * x + b1 * x1[channel] + b2 * x2[channel] -
                   a1 * y1[channel] - a2 * y2[channel];
  x2[channel] = x1[channel];
  x1[channel] = x;
  y2[channel] = y1[channel];
  y1[channel] = y;
  return static_cast<float>(y);
}

void ThreeBandEq::configure(int32_t sampleRate) {
  sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
  // One-pole smoothing coefficient for the configured rate.
  smoothing_ = std::exp(-1.0f / (kSmoothingSeconds * static_cast<float>(sampleRate_)));
  reset();
}

void ThreeBandEq::reset() {
  lowShelf_.reset();
  midPeak_.reset();
  highShelf_.reset();
  currentLow_ = targetLow_;
  currentMid_ = targetMid_;
  currentHigh_ = targetHigh_;
  updateCoefficients();
}

void ThreeBandEq::setBandGains(float low, float mid, float high) {
  targetLow_ = std::clamp(low, 0.0f, 1.0f);
  targetMid_ = std::clamp(mid, 0.0f, 1.0f);
  targetHigh_ = std::clamp(high, 0.0f, 1.0f);
}

void ThreeBandEq::snapToTargets() {
  currentLow_ = targetLow_;
  currentMid_ = targetMid_;
  currentHigh_ = targetHigh_;
  updateCoefficients();
}

bool ThreeBandEq::isNeutral() const {
  return isNeutralGain(currentLow_) && isNeutralGain(currentMid_) &&
         isNeutralGain(currentHigh_) && isNeutralGain(targetLow_) &&
         isNeutralGain(targetMid_) && isNeutralGain(targetHigh_);
}

void ThreeBandEq::updateCoefficients() {
  const double fs = static_cast<double>(sampleRate_);

  // Low shelf (RBJ cookbook).
  {
    const double gainDb = bandGainToDecibels(currentLow_);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * kLowShelfHz / fs;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha =
        sinW0 / 2.0 * std::sqrt((a + 1.0 / a) * (1.0 / kShelfSlope - 1.0) + 2.0);
    const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;

    const double a0 = (a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha;
    lowShelf_.b0 = a * ((a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha) / a0;
    lowShelf_.b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosW0) / a0;
    lowShelf_.b2 = a * ((a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
    lowShelf_.a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosW0) / a0;
    lowShelf_.a2 = ((a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
  }

  // Mid bell.
  {
    const double gainDb = bandGainToDecibels(currentMid_);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * kMidPeakHz / fs;
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * kMidQ);

    const double a0 = 1.0 + alpha / a;
    midPeak_.b0 = (1.0 + alpha * a) / a0;
    midPeak_.b1 = -2.0 * cosW0 / a0;
    midPeak_.b2 = (1.0 - alpha * a) / a0;
    midPeak_.a1 = -2.0 * cosW0 / a0;
    midPeak_.a2 = (1.0 - alpha / a) / a0;
  }

  // High shelf.
  {
    const double gainDb = bandGainToDecibels(currentHigh_);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * kHighShelfHz / fs;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha =
        sinW0 / 2.0 * std::sqrt((a + 1.0 / a) * (1.0 / kShelfSlope - 1.0) + 2.0);
    const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;

    const double a0 = (a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha;
    highShelf_.b0 = a * ((a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha) / a0;
    highShelf_.b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW0) / a0;
    highShelf_.b2 = a * ((a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
    highShelf_.a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosW0) / a0;
    highShelf_.a2 = ((a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
  }
}

void ThreeBandEq::process(float* interleaved, size_t frameCount) {
  // Fully neutral and not moving: leave the samples untouched. This is what
  // makes an unautomated transition bit-exact rather than merely close.
  if (isNeutral()) return;

  for (size_t frame = 0; frame < frameCount; ++frame) {
    // Smooth towards the targets, then rebuild coefficients. Recomputing per
    // sample is affordable here - one voice, three biquads - and avoids the
    // zipper noise that block-rate updates produce on a multi-second sweep.
    const float previousLow = currentLow_;
    const float previousMid = currentMid_;
    const float previousHigh = currentHigh_;

    currentLow_ = targetLow_ + (currentLow_ - targetLow_) * smoothing_;
    currentMid_ = targetMid_ + (currentMid_ - targetMid_) * smoothing_;
    currentHigh_ = targetHigh_ + (currentHigh_ - targetHigh_) * smoothing_;

    if (std::abs(currentLow_ - previousLow) > 1e-6f ||
        std::abs(currentMid_ - previousMid) > 1e-6f ||
        std::abs(currentHigh_ - previousHigh) > 1e-6f) {
      updateCoefficients();
    }

    for (size_t channel = 0; channel < 2; ++channel) {
      float sample = interleaved[frame * 2 + channel];
      sample = lowShelf_.process(sample, channel);
      sample = midPeak_.process(sample, channel);
      sample = highShelf_.process(sample, channel);
      interleaved[frame * 2 + channel] = sample;
    }
  }
}

}  // namespace aidj
