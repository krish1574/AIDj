#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "AudioTypes.h"
#include "IDecoder.h"
#include "dsp/Resampler.h"
#include "dsp/TimeStretch.h"
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

  /**
   * Opens the source and starts filling. Synchronous open, async fill.
   *
   * `startMs` cues playback to a position, which is what lets a transition
   * drop into a track at its first downbeat rather than its first sample.
   */
  EngineError start(const std::string& uri, double startMs = 0.0);
  void stop();

  DecodedFormat format() const { return format_; }
  bool isRunning() const { return running_.load(std::memory_order_relaxed); }

  /**
   * Sets the playback rate for beat matching. 1.0 is untouched.
   *
   * Applied on the decode thread rather than the audio thread, which is the
   * only place it can go: WSOLA consumes a variable amount of input per output
   * frame, and anything with that property cannot live in a callback that must
   * produce exactly N frames without allocating.
   *
   * Takes effect for audio decoded after this call. Audio already in the ring
   * keeps the previous rate, so the ratio must be set before a voice is primed
   * for a transition, not during one.
   */
  void setTempoRatio(double ratio);
  double tempoRatio() const { return tempoRatio_.load(std::memory_order_relaxed); }

 private:
  void run();

  Voice& voice_;
  std::unique_ptr<IDecoder> decoder_;
  Resampler resampler_;
  /** After the resampler, so it always works at the engine's rate. */
  TimeStretch stretch_;
  DecodedFormat format_{};
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<double> tempoRatio_{1.0};
  /** Frames still to discard when cueing without decoder seek support. */
  int64_t discardFrames_ = 0;
  std::vector<float> decodeBuffer_;
  std::vector<float> resampledBuffer_;
  std::vector<float> stretchedBuffer_;
};

}  // namespace aidj
