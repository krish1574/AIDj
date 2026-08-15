#pragma once

#include <cstdint>

namespace aidj {

/**
 * Everything platform-specific about getting samples to a speaker sits behind
 * this. Android implements it with Oboe; iOS will implement it with
 * AVAudioEngine later without the mixer or the analysis code changing.
 */
class IAudioRenderer {
 public:
  virtual ~IAudioRenderer() = default;

  /**
   * Called on the realtime audio thread. Must not allocate, lock, block, do
   * file I/O, or call into the JVM. Writes `frameCount` interleaved stereo
   * frames into `output`.
   */
  virtual void render(float* output, int32_t frameCount) = 0;
};

class IAudioOutput {
 public:
  virtual ~IAudioOutput() = default;

  virtual bool start(IAudioRenderer* renderer) = 0;
  virtual void stop() = 0;

  /** Cumulative buffer underruns reported by the platform since start. */
  virtual int32_t underrunCount() const = 0;
  virtual int32_t framesPerBurst() const = 0;
  /** Negative when the platform cannot report latency. */
  virtual double outputLatencyMs() const = 0;
};

}  // namespace aidj
