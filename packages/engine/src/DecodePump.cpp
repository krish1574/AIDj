#include "DecodePump.h"

#include <chrono>
#include <utility>

namespace aidj {

namespace {
/** Decode granularity. Small enough to stay responsive to stop requests. */
constexpr size_t kDecodeChunkSamples = 8192;

/**
 * A voice is "primed" once it holds this much audio. Half a second is far more
 * than a callback needs but keeps the very first buffer safe on a cold,
 * throttled device.
 */
constexpr size_t kPrimeSamples = kEngineSampleRate * kEngineChannelCount / 2;

/** Keep the ring this full; above it the thread idles instead of spinning. */
constexpr double kRingHighWaterFraction = 0.9;

constexpr auto kIdleSleep = std::chrono::milliseconds(10);
}  // namespace

DecodePump::DecodePump(Voice& voice, std::unique_ptr<IDecoder> decoder)
    : voice_(voice), decoder_(std::move(decoder)) {
  decodeBuffer_.resize(kDecodeChunkSamples);
  resampledBuffer_.reserve(kDecodeChunkSamples * 2);
}

DecodePump::~DecodePump() { stop(); }

EngineError DecodePump::start(const std::string& uri) {
  stop();

  const EngineError error = decoder_->open(uri);
  if (error != EngineError::None) return error;

  format_ = decoder_->format();
  if (format_.sampleRate <= 0 || format_.channelCount <= 0) {
    decoder_->close();
    return EngineError::DecoderUnsupportedFormat;
  }

  resampler_.configure(format_.sampleRate, format_.channelCount,
                       kEngineSampleRate);

  voice_.ring.reset();
  voice_.endOfStream.store(false, std::memory_order_relaxed);
  voice_.primed.store(false, std::memory_order_relaxed);
  voice_.framesRendered.store(0, std::memory_order_relaxed);
  voice_.durationFrames.store(
      format_.durationMs * kEngineSampleRate / 1000, std::memory_order_relaxed);

  stopRequested_.store(false, std::memory_order_relaxed);
  running_.store(true, std::memory_order_relaxed);
  thread_ = std::thread([this] { run(); });
  return EngineError::None;
}

void DecodePump::stop() {
  stopRequested_.store(true, std::memory_order_relaxed);
  if (thread_.joinable()) thread_.join();
  running_.store(false, std::memory_order_relaxed);
  decoder_->close();
}

void DecodePump::run() {
  const size_t highWater =
      static_cast<size_t>(static_cast<double>(voice_.ring.capacity()) *
                          kRingHighWaterFraction);
  bool sourceExhausted = false;

  while (!stopRequested_.load(std::memory_order_relaxed)) {
    if (voice_.ring.sizeAvailableToRead() >= highWater) {
      std::this_thread::sleep_for(kIdleSleep);
      continue;
    }

    if (!sourceExhausted && resampledBuffer_.empty()) {
      const size_t decoded =
          decoder_->decode(decodeBuffer_.data(), decodeBuffer_.size());
      if (decoded == 0) {
        sourceExhausted = true;
        resampler_.finish(resampledBuffer_);
      } else {
        resampler_.process(decodeBuffer_.data(), decoded, resampledBuffer_);
      }
    }

    if (!resampledBuffer_.empty()) {
      const size_t written =
          voice_.ring.write(resampledBuffer_.data(), resampledBuffer_.size());
      resampledBuffer_.erase(resampledBuffer_.begin(),
                             resampledBuffer_.begin() +
                                 static_cast<long>(written));
    } else if (sourceExhausted) {
      voice_.endOfStream.store(true, std::memory_order_release);
      break;
    }

    if (!voice_.primed.load(std::memory_order_relaxed) &&
        voice_.ring.sizeAvailableToRead() >= kPrimeSamples) {
      voice_.primed.store(true, std::memory_order_release);
    }
  }

  // A short track may finish before ever reaching the prime threshold; it is
  // still ready to play, so mark it primed rather than leaving it stuck.
  voice_.primed.store(true, std::memory_order_release);
  running_.store(false, std::memory_order_release);
}

}  // namespace aidj
