#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aidj {

/**
 * Interleaved stereo resampler using Catmull-Rom cubic interpolation, plus
 * mono-to-stereo duplication.
 *
 * KNOWN LIMITATION, stated plainly: cubic interpolation is not a
 * bandwidth-correct resampler. At the common 44.1 -> 48 kHz ratio its
 * imaging/aliasing artefacts sit roughly 60-70 dB down, which is fine for
 * bring-up and for verifying the playback path, but it is not the quality this
 * product should ship with. It must be replaced with a windowed-sinc polyphase
 * FIR before any listening evaluation of transition quality, because
 * resampling artefacts would contaminate that judgement. Tracked in
 * docs/known-limitations.md.
 */
class Resampler {
 public:
  void configure(int32_t sourceRate, int32_t sourceChannels, int32_t targetRate);

  /**
   * Consumes `sourceSampleCount` interleaved source samples and appends the
   * converted stereo result to `output`. Retains the trailing samples needed
   * for interpolation continuity across calls, so there is no seam at buffer
   * boundaries.
   */
  void process(const float* source, size_t sourceSampleCount,
               std::vector<float>& output);

  /** Flushes the remaining tail once the source is exhausted. */
  void finish(std::vector<float>& output);

  void reset();

  bool isPassThrough() const { return passThrough_; }

 private:
  static float catmullRom(float p0, float p1, float p2, float p3, float t);

  int32_t sourceChannels_ = 2;
  double ratio_ = 1.0;  // source frames consumed per output frame
  double position_ = 0.0;
  bool passThrough_ = true;
  /** History of source frames, de-interleaved into stereo pairs. */
  std::vector<float> history_;
};

}  // namespace aidj
