#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aidj {

/**
 * Pitch-preserving time stretch by WSOLA (waveform similarity overlap-add).
 *
 * WHY THIS AND NOT A LIBRARY
 *
 * Rubber Band is GPL-or-paid-commercial, which is unusable in a closed app.
 * Signalsmith Stretch is MIT and excellent, but the requirement here is
 * narrow: beat-matching two records needs at most about +-8%, and in that
 * range WSOLA is essentially transparent on rhythmic material. A phase vocoder
 * earns its cost at extreme ratios that this product deliberately refuses to
 * ask for - the transition planner falls back to a plain crossfade instead.
 *
 * If listening tests later show WSOLA smearing on sustained material, this
 * class is the seam to swap: it owns no policy, only the conversion.
 *
 * HOW IT WORKS
 *
 * Overlap-add of fixed-length windows, where each new window is taken not from
 * the nominal position but from wherever nearby the waveform best matches what
 * was already written. That search is the whole difference between WSOLA and
 * naive overlap-add: without it, the splice points fall at arbitrary phases
 * and produce the metallic warble that gives cheap tempo change away.
 */
class TimeStretch {
 public:
  void configure(int32_t sampleRate, int32_t channels);

  /**
   * Sets the speed multiplier. 1.0 is untouched; 1.05 plays 5% faster while
   * keeping pitch. Values outside a sane range are clamped rather than
   * producing nonsense.
   */
  void setRatio(double ratio);

  double ratio() const { return ratio_; }

  /** Feeds interleaved input. Output is appended to `output`. */
  void process(const float* interleaved, size_t frameCount,
               std::vector<float>& output);

  /** Emits whatever remains once input has ended. */
  void finish(std::vector<float>& output);

  void reset();

  /** True when the ratio is 1 and the stretcher is a pass-through. */
  bool isPassThrough() const;

 private:
  /** Best offset near `nominal` by cross-correlation against the overlap tail. */
  size_t findBestOffset(size_t nominal) const;

  int32_t sampleRate_ = 48000;
  int32_t channels_ = 2;
  double ratio_ = 1.0;

  size_t windowFrames_ = 0;
  size_t overlapFrames_ = 0;
  size_t searchFrames_ = 0;

  std::vector<float> input_;    // interleaved, pending
  std::vector<float> overlap_;  // tail of the last window, for the next fade
  std::vector<float> window_;   // raised-cosine fade shape

  /** Read cursor within input_, in frames, as a fraction of nominal hop. */
  double readPosition_ = 0.0;
  bool primed_ = false;
};

}  // namespace aidj
