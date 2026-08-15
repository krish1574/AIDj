#include "Resampler.h"

#include <algorithm>
#include <cmath>

#include "../AudioTypes.h"

namespace aidj {
namespace {

constexpr double kPi = 3.14159265358979323846;

/**
 * Base filter half-length in source frames, before any downsample widening.
 *
 * 16 measured identically to 32 on the near-Nyquist imaging test (-59.95 dB
 * either way), so the transition band is not what limits accuracy there -
 * phase quantisation is, and that is addressed by interpolating between
 * adjacent phases below. Kept at 16 because the extra taps bought nothing and
 * cost CPU on every output sample.
 */
constexpr size_t kBaseHalfTaps = 16;
/** Fractional-delay resolution. See the note on phase quantisation in the header. */
constexpr size_t kPhases = 512;
/** Kaiser beta. ~90 dB stopband, the standard choice for audio SRC. */
constexpr double kKaiserBeta = 8.6;

/**
 * Cutoff as a fraction of Nyquist.
 *
 * A filter cut exactly at Nyquist has half its transition band above it, so
 * near-Nyquist content images straight through. Measured at 44.1 -> 48 kHz
 * with a 20 kHz tone: spurious energy at -24.8 dB with no margin, versus below
 * -70 dB with this value. That is the difference between an obvious whistle
 * and nothing.
 *
 * A 1 kHz test tone cannot see this at all - it has no energy near the
 * transition band - which is why the near-Nyquist test exists alongside it.
 *
 * The cost is content above ~20.3 kHz at 44.1 kHz input, which is above the
 * hearing range and, on the material this app plays, is mostly codec noise.
 */
constexpr double kCutoffScale = 0.92;

/** Zeroth-order modified Bessel function of the first kind, series form. */
double besselI0(double x) {
  double sum = 1.0;
  double term = 1.0;
  const double halfXSquared = (x * x) / 4.0;
  // Converges quickly for the arguments a Kaiser window uses; 32 terms is far
  // more than enough at beta 8.6 and costs nothing, since this runs once.
  for (int k = 1; k < 32; ++k) {
    term *= halfXSquared / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

double sinc(double x) {
  if (std::abs(x) < 1e-9) return 1.0;
  const double piX = kPi * x;
  return std::sin(piX) / piX;
}

}  // namespace

void Resampler::configure(int32_t sourceRate, int32_t sourceChannels,
                          int32_t targetRate) {
  sourceChannels_ = sourceChannels < 1 ? 1 : sourceChannels;
  ratio_ = static_cast<double>(sourceRate) / static_cast<double>(targetRate);
  ratioWhole_ = static_cast<size_t>(ratio_);
  ratioFraction_ = ratio_ - static_cast<double>(ratioWhole_);
  passThrough_ = (sourceRate == targetRate && sourceChannels_ == 2);

  if (!passThrough_) buildFilterTable(ratio_);
  reset();
}

void Resampler::buildFilterTable(double ratio) {
  // Downsampling: the new Nyquist is lower, so the cutoff has to come down with
  // it or content above it folds back as aliasing. Upsampling needs no such
  // reduction - the source is already band-limited to its own Nyquist.
  const double downsampleFactor = std::max(1.0, ratio);
  const double cutoff = 0.5 * kCutoffScale / downsampleFactor;

  // Widen the filter by the same factor, so the transition band stays as sharp
  // in the output as it would be at unity ratio. Capped: extreme ratios are not
  // worth unbounded CPU, and 8x covers anything a music file will contain.
  halfTaps_ = static_cast<size_t>(
      std::ceil(static_cast<double>(kBaseHalfTaps) *
                std::min(downsampleFactor, 8.0)));
  taps_ = halfTaps_ * 2;
  phases_ = kPhases;

  // One row beyond the phase count, computed at fraction 1.0, so the final
  // phase has a genuine successor to interpolate towards instead of blending
  // with itself.
  table_.assign((phases_ + 1) * taps_, 0.0f);
  const double bessel = besselI0(kKaiserBeta);

  for (size_t phase = 0; phase <= phases_; ++phase) {
    const double fraction =
        static_cast<double>(phase) / static_cast<double>(phases_);

    double sum = 0.0;
    for (size_t tap = 0; tap < taps_; ++tap) {
      // Distance from the interpolation point to this tap, in source frames.
      const double offset = static_cast<double>(tap) -
                            static_cast<double>(halfTaps_ - 1) - fraction;

      // Kaiser window over the filter support.
      const double normalised = offset / static_cast<double>(halfTaps_);
      double window = 0.0;
      if (std::abs(normalised) <= 1.0) {
        window = besselI0(kKaiserBeta *
                          std::sqrt(std::max(0.0, 1.0 - normalised * normalised))) /
                 bessel;
      }

      const double value = 2.0 * cutoff * sinc(2.0 * cutoff * offset) * window;
      table_[phase * taps_ + tap] = static_cast<float>(value);
      sum += value;
    }

    // Normalise for exactly unity DC gain. Without this the windowed sinc is
    // slightly off unity and a full-scale input can exceed full scale out.
    if (std::abs(sum) > 1e-12) {
      for (size_t tap = 0; tap < taps_; ++tap) {
        table_[phase * taps_ + tap] /= static_cast<float>(sum);
      }
    }
  }
}

void Resampler::reset() {
  history_.clear();
  positionFraction_ = 0.0;
  if (passThrough_) {
    positionFrame_ = 0;
    return;
  }

  // Prime with the filter's left context as silence, so the first real sample
  // is filtered rather than read out of bounds.
  history_.assign((halfTaps_ - 1) * 2, 0.0f);
  positionFrame_ = halfTaps_ - 1;
}

void Resampler::process(const float* source, size_t sourceSampleCount,
                        std::vector<float>& output) {
  if (passThrough_) {
    output.insert(output.end(), source, source + sourceSampleCount);
    return;
  }

  const size_t channels = static_cast<size_t>(sourceChannels_);
  const size_t incomingFrames = sourceSampleCount / channels;

  // Normalise to stereo as we append: mono duplicates, >2 channels keeps the
  // first two. Anything beyond stereo is out of scope for a music player.
  history_.reserve(history_.size() + incomingFrames * 2);
  for (size_t frame = 0; frame < incomingFrames; ++frame) {
    const float left = source[frame * channels];
    const float right = channels >= 2 ? source[frame * channels + 1] : left;
    history_.push_back(left);
    history_.push_back(right);
  }

  const size_t availableFrames = history_.size() / 2;

  // An output sample needs taps up to halfTaps_ frames to the right of the
  // current frame, so we can only emit while that much future input is present.
  while (positionFrame_ + halfTaps_ < availableFrames) {
    // Interpolate between the two nearest phases rather than snapping to one.
    //
    // Snapping quantises the fractional delay to 1/phases_ of a sample. That
    // error is inaudible at low frequencies but scales with frequency: at
    // 20 kHz it put spurious content at -60 dB, and no amount of extra filter
    // taps helped because the transition band was not the limit. Interpolating
    // costs one extra multiply-add per tap and removes it.
    const double scaled = positionFraction_ * static_cast<double>(phases_);
    const size_t phase = std::min(phases_ - 1, static_cast<size_t>(scaled));
    const size_t nextPhase = phase + 1;  // guard row exists at index phases_
    const float blend = static_cast<float>(scaled - static_cast<double>(phase));

    const float* coefficients = &table_[phase * taps_];
    const float* nextCoefficients = &table_[nextPhase * taps_];

    // First tap sits halfTaps_-1 frames before the current frame.
    const size_t start = positionFrame_ - (halfTaps_ - 1);

    float left = 0.0f;
    float right = 0.0f;
    for (size_t tap = 0; tap < taps_; ++tap) {
      const float weight =
          coefficients[tap] + (nextCoefficients[tap] - coefficients[tap]) * blend;
      left += history_[(start + tap) * 2] * weight;
      right += history_[(start + tap) * 2 + 1] * weight;
    }

    output.push_back(left);
    output.push_back(right);

    positionFrame_ += ratioWhole_;
    positionFraction_ += ratioFraction_;
    if (positionFraction_ >= 1.0) {
      positionFraction_ -= 1.0;
      positionFrame_ += 1;
    }
  }

  // Drop consumed history, keeping the filter's left context. Only the frame
  // index moves; the fraction is untouched, which is what makes the output
  // independent of how the input was chunked.
  if (positionFrame_ > halfTaps_ - 1) {
    const size_t dropFrames = positionFrame_ - (halfTaps_ - 1);
    history_.erase(history_.begin(),
                   history_.begin() + static_cast<long>(dropFrames * 2));
    positionFrame_ -= dropFrames;
  }
}

void Resampler::finish(std::vector<float>& output) {
  if (passThrough_) return;

  // Pad with the filter's right context as silence so the final real samples
  // are filtered rather than truncated, which would click at the end of a track.
  const std::vector<float> padding(halfTaps_ * 2 * 2, 0.0f);
  process(padding.data(), padding.size(), output);
}

}  // namespace aidj
