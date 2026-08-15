#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "IDecoder.h"

namespace aidj::test {

/**
 * Generates a deterministic sine tone instead of reading a file.
 *
 * This is a test double for the platform decoder, not a stand-in for real
 * decoding: it exists so the ring buffer, resampler, mixer and state machine
 * can be exercised on a host with a signal whose exact expected value is known
 * at every sample. Real decode correctness can only be tested on a device with
 * real files, and is verified there.
 *
 * The URI encodes the tone: "test://sine?hz=440&rate=44100&ch=2&ms=2000".
 */
class TestDecoder final : public IDecoder {
 public:
  EngineError open(const std::string& uri) override;
  DecodedFormat format() const override { return format_; }
  size_t decode(float* destination, size_t maxSamples) override;
  void close() override;

  /** Number of TestDecoder instances currently open - catches leaks. */
  static std::atomic<int>& openCount();

 private:
  DecodedFormat format_{};
  double frequencyHz_ = 440.0;
  int64_t framesProduced_ = 0;
  int64_t totalFrames_ = 0;
  bool open_ = false;
};

}  // namespace aidj::test
