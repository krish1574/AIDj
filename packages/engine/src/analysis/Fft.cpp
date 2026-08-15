#include "analysis/Fft.h"

#include <cassert>
#include <cmath>

namespace aidj {
namespace {

constexpr float kPi = 3.14159265358979323846f;

bool isPowerOfTwo(size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

size_t log2Size(size_t value) {
  size_t levels = 0;
  while ((size_t{1} << levels) < value) levels += 1;
  return levels;
}

}  // namespace

Fft::Fft(size_t size)
    : size_(size), levels_(log2Size(size)), scratch_(size) {
  assert(isPowerOfTwo(size) && "FFT size must be a power of two");

  // Precompute half the twiddles; the butterfly only ever needs e^(-2*pi*i*k/N)
  // for k < N/2.
  twiddles_.resize(size_ / 2);
  for (size_t k = 0; k < size_ / 2; ++k) {
    const float angle = -2.0f * kPi * static_cast<float>(k) /
                        static_cast<float>(size_);
    twiddles_[k] = std::complex<float>(std::cos(angle), std::sin(angle));
  }
}

void Fft::forward(std::complex<float>* data) const {
  if (size_ <= 1) return;

  // Bit-reversal permutation.
  for (size_t i = 1, j = 0; i < size_; ++i) {
    size_t bit = size_ >> 1;
    for (; (j & bit) != 0; bit >>= 1) j ^= bit;
    j |= bit;
    if (i < j) std::swap(data[i], data[j]);
  }

  for (size_t length = 2; length <= size_; length <<= 1) {
    const size_t half = length >> 1;
    const size_t step = size_ / length;
    for (size_t start = 0; start < size_; start += length) {
      for (size_t offset = 0; offset < half; ++offset) {
        const std::complex<float> twiddle = twiddles_[offset * step];
        const std::complex<float> upper = data[start + offset];
        const std::complex<float> lower = data[start + offset + half] * twiddle;
        data[start + offset] = upper + lower;
        data[start + offset + half] = upper - lower;
      }
    }
  }
}

void Fft::magnitudeSpectrum(const float* input, float* magnitudes) const {
  for (size_t i = 0; i < size_; ++i) {
    scratch_[i] = std::complex<float>(input[i], 0.0f);
  }

  forward(scratch_.data());

  const size_t bins = size_ / 2 + 1;
  for (size_t i = 0; i < bins; ++i) {
    magnitudes[i] = std::abs(scratch_[i]);
  }
}

std::vector<float> hannWindow(size_t size) {
  std::vector<float> window(size);
  if (size == 1) {
    window[0] = 1.0f;
    return window;
  }
  // Periodic (not symmetric): correct for STFT analysis, where the window is
  // one period of a repeating sequence rather than a standalone taper.
  for (size_t i = 0; i < size; ++i) {
    window[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) /
                                        static_cast<float>(size)));
  }
  return window;
}

}  // namespace aidj
