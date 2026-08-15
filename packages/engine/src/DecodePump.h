#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "AudioTypes.h"
#include "IDecoder.h"
#include "dsp/Resampler.h"
#include "mix/Mixer.h"

namespace aidj {

/**
 * Owns one decoder thread feeding one voice's ring buffer.
 *
 * This is the "prepare ahead" primitive the continuous mix engine is built on:
 * a voice can be filled and primed while the other voice is audible, so a
 * transition never waits on I/O. Milestone 1 drives it manually; Milestone 6
 * drives it from the queue.
 *
 * The thread decodes until the ring is nearly full, then sleeps in short
 * intervals. It never blocks the audio thread and never touches mixer state
 * other than its own voice's producer-side fields.
 */
class DecodePump {
 public:
  DecodePump(Voice& voice, std::unique_ptr<IDecoder> decoder);
  ~DecodePump();

  DecodePump(const DecodePump&) = delete;
  DecodePump& operator=(const DecodePump&) = delete;

  /** Opens the source and starts filling. Synchronous open, async fill. */
  EngineError start(const std::string& uri);
  void stop();

  DecodedFormat format() const { return format_; }
  bool isRunning() const { return running_.load(std::memory_order_relaxed); }

 private:
  void run();

  Voice& voice_;
  std::unique_ptr<IDecoder> decoder_;
  Resampler resampler_;
  DecodedFormat format_{};
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  std::vector<float> decodeBuffer_;
  std::vector<float> resampledBuffer_;
};

}  // namespace aidj
