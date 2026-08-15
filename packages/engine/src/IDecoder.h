#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "AudioTypes.h"

namespace aidj {

struct DecodedFormat {
  int32_t sampleRate = 0;
  int32_t channelCount = 0;
  int64_t durationMs = 0;
};

/**
 * Pull-based decoder. Implementations produce interleaved float samples at the
 * source's own rate and channel count; resampling to the engine format is done
 * one layer up so that a platform decoder never has to care.
 */
class IDecoder {
 public:
  virtual ~IDecoder() = default;

  virtual EngineError open(const std::string& uri) = 0;
  virtual DecodedFormat format() const = 0;

  /**
   * Seeks to `positionMs`.
   *
   * Optional: the default reports failure, and callers are expected to fall
   * back to decoding and discarding. That fallback is not merely a nicety -
   * cueing 30 minutes into a long mix by decoding through it would take
   * minutes, so a decoder that can seek properly is the difference between a
   * usable cue point and an unusable one.
   */
  virtual EngineError seek(double positionMs) {
    (void)positionMs;
    return EngineError::DecoderIoError;
  }

  /**
   * Fills up to `maxSamples` interleaved floats. Returns the count written.
   * Returns 0 once the stream is exhausted.
   */
  virtual size_t decode(float* destination, size_t maxSamples) = 0;

  virtual void close() = 0;
};

/** Provided per platform; Android returns a MediaCodec-backed decoder. */
std::unique_ptr<IDecoder> createPlatformDecoder();

}  // namespace aidj
