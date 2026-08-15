#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace aidj {

/**
 * Lock-free single-producer / single-consumer float ring buffer.
 *
 * Producer is exactly one decoder thread; consumer is exactly one audio
 * callback. This is the only channel between them. It never allocates after
 * construction and never blocks - the audio callback must not wait on a
 * decoder that is behind, it must read what is there and report an underrun.
 *
 * Capacity is rounded up to a power of two so index wrapping is a mask.
 */
class RingBuffer {
 public:
  explicit RingBuffer(size_t minimumCapacity)
      : capacity_(roundUpToPowerOfTwo(minimumCapacity)),
        mask_(capacity_ - 1),
        data_(capacity_, 0.0f) {}

  /** Producer side. Returns the number of samples actually written. */
  size_t write(const float* source, size_t count) {
    const size_t writeIndex = writeIndex_.load(std::memory_order_relaxed);
    const size_t readIndex = readIndex_.load(std::memory_order_acquire);
    const size_t available = capacity_ - (writeIndex - readIndex) - 1;
    const size_t toWrite = count < available ? count : available;

    for (size_t i = 0; i < toWrite; ++i) {
      data_[(writeIndex + i) & mask_] = source[i];
    }
    writeIndex_.store(writeIndex + toWrite, std::memory_order_release);
    return toWrite;
  }

  /**
   * Consumer side. Reads exactly `count` samples, zero-filling any shortfall
   * so the callback always has a full buffer to hand to the mixer. Returns the
   * number of real samples read; anything less than `count` is an underrun.
   */
  size_t readOrSilence(float* destination, size_t count) {
    const size_t readIndex = readIndex_.load(std::memory_order_relaxed);
    const size_t writeIndex = writeIndex_.load(std::memory_order_acquire);
    const size_t available = writeIndex - readIndex;
    const size_t toRead = count < available ? count : available;

    for (size_t i = 0; i < toRead; ++i) {
      destination[i] = data_[(readIndex + i) & mask_];
    }
    if (toRead < count) {
      std::memset(destination + toRead, 0, (count - toRead) * sizeof(float));
    }
    readIndex_.store(readIndex + toRead, std::memory_order_release);
    return toRead;
  }

  size_t sizeAvailableToRead() const {
    return writeIndex_.load(std::memory_order_acquire) -
           readIndex_.load(std::memory_order_acquire);
  }

  size_t capacity() const { return capacity_; }

  /**
   * Only safe when the producer is stopped and the consumer is not running -
   * i.e. between tracks, never mid-playback.
   */
  void reset() {
    readIndex_.store(0, std::memory_order_relaxed);
    writeIndex_.store(0, std::memory_order_relaxed);
  }

 private:
  static size_t roundUpToPowerOfTwo(size_t value) {
    size_t result = 1;
    while (result < value) result <<= 1;
    return result;
  }

  const size_t capacity_;
  const size_t mask_;
  std::vector<float> data_;
  std::atomic<size_t> readIndex_{0};
  std::atomic<size_t> writeIndex_{0};
};

}  // namespace aidj
