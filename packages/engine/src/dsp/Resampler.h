#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aidj {

/**
 * Interleaved stereo resampler using a Kaiser-windowed sinc polyphase FIR,
 * plus mono-to-stereo duplication.
 *
 * This replaces an earlier Catmull-Rom cubic interpolator whose imaging and
 * aliasing artefacts sat only ~60-70 dB down. That was adequate for bringing
 * playback up, but not for judging how a *transition* sounds: artefacts at
 * that level are audible in quiet passages and would have contaminated exactly
 * the listening evaluation the transition engine has to be tuned against.
 *
 * Design, and why each choice:
 *
 * - Kaiser window, beta 8.6. Gives roughly 90 dB stopband attenuation, which
 *   puts resampling artefacts below the noise floor of any real recording.
 * - 32 taps per output sample. Enough for a transition band narrow enough to
 *   preserve content near Nyquist at the 44.1 -> 48 kHz ratio that dominates
 *   real libraries.
 * - 512 phases. The phase quantisation error is then far smaller than the
 *   filter's own stopband, so no inter-phase interpolation is needed.
 * - When downsampling, the cutoff moves below the *output* Nyquist and the
 *   filter widens in proportion. Without that, decimation folds everything
 *   above the new Nyquist back into the audible band, which is the one
 *   resampling mistake that is unmistakably audible.
 *
 * Each phase's coefficients are normalised to sum to one, so DC gain is exactly
 * unity and a full-scale input cannot come out louder than it went in.
 */
class Resampler {
 public:
  void configure(int32_t sourceRate, int32_t sourceChannels, int32_t targetRate);

  /**
   * Consumes `sourceSampleCount` interleaved source samples and appends the
   * converted stereo result to `output`. Retains the trailing samples needed
   * for filter continuity across calls, so there is no seam at buffer
   * boundaries.
   */
  void process(const float* source, size_t sourceSampleCount,
               std::vector<float>& output);

  /** Flushes the remaining tail once the source is exhausted. */
  void finish(std::vector<float>& output);

  void reset();

  bool isPassThrough() const { return passThrough_; }

  /** Filter half-length in source frames. Exposed for tests and latency maths. */
  size_t halfTaps() const { return halfTaps_; }

 private:
  void buildFilterTable(double ratio);

  int32_t sourceChannels_ = 2;
  double ratio_ = 1.0;  // source frames consumed per output frame

  /**
   * Read position, split into an exact frame index and a fraction in [0, 1).
   *
   * Deliberately not one double. Trimming consumed history subtracts a
   * different integer at a different time depending on how input was chunked,
   * and a single double accumulates those operations in a different order each
   * way. The results differ by an ULP, which is enough to select a
   * neighbouring filter phase and make a 512-phase resampler produce different
   * output for the same audio depending on buffer sizes - an audible seam at
   * chunk boundaries. Keeping the fraction independent of trimming makes the
   * output identical however the input is split.
   */
  size_t positionFrame_ = 0;
  double positionFraction_ = 0.0;

  /** ratio_ split the same way, so advancing never touches the integer part. */
  size_t ratioWhole_ = 0;
  double ratioFraction_ = 0.0;

  bool passThrough_ = true;

  /** Interleaved stereo history, including the filter's left context. */
  std::vector<float> history_;

  /** phases x taps, row-major. Row p is the filter for fractional offset p/N. */
  std::vector<float> table_;
  size_t phases_ = 0;
  size_t taps_ = 0;
  size_t halfTaps_ = 0;
};

}  // namespace aidj
