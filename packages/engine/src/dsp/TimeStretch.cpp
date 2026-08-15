#include "dsp/TimeStretch.h"

#include <algorithm>
#include <cmath>

namespace aidj {
namespace {

constexpr double kPi = 3.14159265358979323846;

/**
 * Window length in milliseconds.
 *
 * Long enough to contain a full cycle of the lowest musical pitch that matters
 * (around 50 Hz needs 20 ms), short enough that a transient is not smeared
 * across a perceptible span. 40 ms is the usual compromise for music.
 */
constexpr double kWindowMs = 40.0;

/** Overlap as a fraction of the window. Half is standard for a cosine fade. */
constexpr double kOverlapFraction = 0.5;

/**
 * How far WSOLA may hunt for a better splice point, in milliseconds.
 *
 * Needs to cover at least one period of the lowest frequency with strong
 * periodicity - a 60 Hz kick is ~17 ms - or the search cannot find the
 * alignment that makes bass splice cleanly.
 */
constexpr double kSearchMs = 20.0;

/** Beyond this, WSOLA artefacts stop being subtle. The planner avoids it. */
constexpr double kMinRatio = 0.5;
constexpr double kMaxRatio = 2.0;

}  // namespace

void TimeStretch::configure(int32_t sampleRate, int32_t channels) {
  sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
  channels_ = channels > 0 ? channels : 2;

  windowFrames_ = static_cast<size_t>(kWindowMs * sampleRate_ / 1000.0);
  overlapFrames_ = static_cast<size_t>(windowFrames_ * kOverlapFraction);
  searchFrames_ = static_cast<size_t>(kSearchMs * sampleRate_ / 1000.0);

  // Raised cosine over the overlap region: equal-power when two of them
  // cross, so the overlap-add does not dip in level.
  window_.resize(overlapFrames_);
  for (size_t i = 0; i < overlapFrames_; ++i) {
    const double phase =
        kPi * static_cast<double>(i) / static_cast<double>(overlapFrames_);
    window_[i] = static_cast<float>(0.5 * (1.0 - std::cos(phase)));
  }

  reset();
}

void TimeStretch::setRatio(double ratio) {
  ratio_ = std::clamp(ratio, kMinRatio, kMaxRatio);
}

bool TimeStretch::isPassThrough() const { return std::abs(ratio_ - 1.0) < 1e-9; }

void TimeStretch::reset() {
  input_.clear();
  overlap_.assign(overlapFrames_ * static_cast<size_t>(channels_), 0.0f);
  readPosition_ = 0.0;
  primed_ = false;
}

size_t TimeStretch::findBestOffset(size_t nominal) const {
  // Without this search, splices land at arbitrary phase and the result
  // warbles. With it, each new window starts where the waveform most closely
  // continues what was just written.
  const size_t channels = static_cast<size_t>(channels_);
  const size_t availableFrames = input_.size() / channels;

  const size_t from = nominal > searchFrames_ ? nominal - searchFrames_ : 0;
  const size_t to = std::min(nominal + searchFrames_,
                             availableFrames > windowFrames_
                                 ? availableFrames - windowFrames_
                                 : 0);
  if (to <= from) return std::min(nominal, to);

  double bestScore = -1e30;
  size_t bestOffset = nominal;

  for (size_t candidate = from; candidate <= to; ++candidate) {
    double score = 0.0;
    // Correlate the previous overlap tail against this candidate's head.
    // Mono-summed: alignment is a rhythmic property, and correlating each
    // channel separately can pick different offsets for a wide stereo image.
    for (size_t i = 0; i < overlapFrames_; ++i) {
      double previous = 0.0;
      double next = 0.0;
      for (size_t channel = 0; channel < channels; ++channel) {
        previous += overlap_[i * channels + channel];
        next += input_[(candidate + i) * channels + channel];
      }
      score += previous * next;
    }
    if (score > bestScore) {
      bestScore = score;
      bestOffset = candidate;
    }
  }

  return bestOffset;
}

void TimeStretch::process(const float* interleaved, size_t frameCount,
                          std::vector<float>& output) {
  const size_t channels = static_cast<size_t>(channels_);

  if (isPassThrough()) {
    output.insert(output.end(), interleaved, interleaved + frameCount * channels);
    return;
  }

  input_.insert(input_.end(), interleaved, interleaved + frameCount * channels);

  const size_t hop = windowFrames_ - overlapFrames_;

  // Analysis advances by hop*ratio while synthesis advances by hop; that
  // difference is the whole of the time change.
  const double analysisHop = static_cast<double>(hop) * ratio_;

  while (true) {
    const size_t availableFrames = input_.size() / channels;
    const size_t nominal = static_cast<size_t>(readPosition_);

    // Need the window plus room for the search on either side.
    if (availableFrames < nominal + windowFrames_ + searchFrames_) break;

    const size_t offset = primed_ ? findBestOffset(nominal) : nominal;
    primed_ = true;

    // Cross-fade the previous tail with this window's head, then emit the
    // steady part of the window and keep its tail for next time.
    for (size_t i = 0; i < overlapFrames_; ++i) {
      const float fadeIn = window_[i];
      const float fadeOut = 1.0f - fadeIn;
      for (size_t channel = 0; channel < channels; ++channel) {
        const float previous = overlap_[i * channels + channel];
        const float next = input_[(offset + i) * channels + channel];
        output.push_back(previous * fadeOut + next * fadeIn);
      }
    }

    for (size_t i = overlapFrames_; i < hop; ++i) {
      for (size_t channel = 0; channel < channels; ++channel) {
        output.push_back(input_[(offset + i) * channels + channel]);
      }
    }

    for (size_t i = 0; i < overlapFrames_; ++i) {
      for (size_t channel = 0; channel < channels; ++channel) {
        overlap_[i * channels + channel] =
            input_[(offset + hop + i) * channels + channel];
      }
    }

    readPosition_ += analysisHop;

    // Drop input that is now behind the search window.
    const size_t keepFrom =
        static_cast<size_t>(readPosition_) > searchFrames_
            ? static_cast<size_t>(readPosition_) - searchFrames_
            : 0;
    if (keepFrom > 0) {
      input_.erase(input_.begin(),
                   input_.begin() + static_cast<long>(keepFrom * channels));
      readPosition_ -= static_cast<double>(keepFrom);
    }
  }
}

void TimeStretch::finish(std::vector<float>& output) {
  if (isPassThrough()) return;

  const size_t channels = static_cast<size_t>(channels_);
  // Emit the retained tail so the last fragment is not simply dropped, which
  // would cut the end of a track mid-waveform.
  for (size_t i = 0; i < overlapFrames_; ++i) {
    const float fadeOut = 1.0f - window_[i];
    for (size_t channel = 0; channel < channels; ++channel) {
      output.push_back(overlap_[i * channels + channel] * fadeOut);
    }
  }
  overlap_.assign(overlapFrames_ * channels, 0.0f);
}

}  // namespace aidj
