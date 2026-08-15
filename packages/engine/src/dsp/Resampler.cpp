#include "Resampler.h"

#include <algorithm>
#include <cmath>

#include "../AudioTypes.h"

namespace aidj {

void Resampler::configure(int32_t sourceRate, int32_t sourceChannels,
                          int32_t targetRate) {
  sourceChannels_ = sourceChannels < 1 ? 1 : sourceChannels;
  ratio_ = static_cast<double>(sourceRate) / static_cast<double>(targetRate);
  passThrough_ = (sourceRate == targetRate && sourceChannels_ == 2);
  reset();
}

void Resampler::reset() {
  position_ = 0.0;
  history_.clear();
  // Three leading zero frames so the very first output sample has the four
  // neighbours Catmull-Rom needs without reading out of bounds.
  history_.assign(3 * 2, 0.0f);
  position_ = 1.0;
}

float Resampler::catmullRom(float p0, float p1, float p2, float p3, float t) {
  const float a = 2.0f * p1;
  const float b = p2 - p0;
  const float c = 2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3;
  const float d = -p0 + 3.0f * p1 - 3.0f * p2 + p3;
  return 0.5f * (a + b * t + c * t * t + d * t * t * t);
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
  // Need one frame beyond the interpolation window before emitting.
  while (position_ + 2.0 < static_cast<double>(availableFrames)) {
    const size_t index = static_cast<size_t>(position_);
    const float t = static_cast<float>(position_ - static_cast<double>(index));

    for (size_t channel = 0; channel < 2; ++channel) {
      const float p0 = history_[(index - 1) * 2 + channel];
      const float p1 = history_[index * 2 + channel];
      const float p2 = history_[(index + 1) * 2 + channel];
      const float p3 = history_[(index + 2) * 2 + channel];
      output.push_back(catmullRom(p0, p1, p2, p3, t));
    }
    position_ += ratio_;
  }

  // Drop consumed history, keeping the one frame of left context Catmull-Rom
  // needs, and rebase the fractional position onto the trimmed buffer.
  const size_t consumed = static_cast<size_t>(position_);
  if (consumed > 1) {
    const size_t dropFrames = consumed - 1;
    history_.erase(history_.begin(),
                   history_.begin() + static_cast<long>(dropFrames * 2));
    position_ -= static_cast<double>(dropFrames);
  }
}

void Resampler::finish(std::vector<float>& output) {
  if (passThrough_) return;
  // Pad with two silent frames so the final real samples can be interpolated
  // rather than truncated, which would leave a click at the end of the track.
  const float padding[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  process(padding, 4, output);
}

}  // namespace aidj
