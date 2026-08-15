#pragma once

#include <cstddef>
#include <complex>
#include <vector>

namespace aidj {

/**
 * Iterative in-place radix-2 FFT.
 *
 * Hand-written rather than vendored. The analysis workload is dominated by
 * decoding, not by transforms - a 2048-point frame every 512 samples over an
 * hour of audio is a few million transforms, which this handles in seconds -
 * and a self-contained implementation avoids pulling a third-party library
 * (and its licence) into the build for a well-understood 100 lines.
 *
 * Sizes must be powers of two; the analysis pipeline only ever asks for them.
 */
class Fft {
 public:
  explicit Fft(size_t size);

  size_t size() const { return size_; }

  /** In-place forward transform. `data` must hold exactly size() elements. */
  void forward(std::complex<float>* data) const;

  /**
   * Magnitude spectrum of a real signal.
   *
   * `input` holds size() real samples; `magnitudes` receives size()/2 + 1
   * bins. This is the only entry point the analysis code uses - it never needs
   * phase, which is why nothing here reconstructs a signal.
   */
  void magnitudeSpectrum(const float* input, float* magnitudes) const;

 private:
  size_t size_;
  size_t levels_;
  std::vector<std::complex<float>> twiddles_;
  mutable std::vector<std::complex<float>> scratch_;
};

/** Periodic Hann window, the standard choice for overlap-add analysis. */
std::vector<float> hannWindow(size_t size);

}  // namespace aidj
