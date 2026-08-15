#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "IAudioOutput.h"

namespace aidj::test {

/**
 * Drives the renderer from a plain thread instead of a real audio device, and
 * keeps every rendered sample so tests can assert on the actual output signal.
 *
 * It runs as fast as it can rather than in realtime - a test that waits for
 * eight real seconds of audio is a test nobody runs.
 */
class NullOutput final : public IAudioOutput {
 public:
  explicit NullOutput(int32_t framesPerCallback = 192);
  ~NullOutput() override;

  bool start(IAudioRenderer* renderer) override;
  void stop() override;

  int32_t underrunCount() const override { return 0; }
  int32_t framesPerBurst() const override { return framesPerCallback_; }
  double outputLatencyMs() const override { return -1.0; }

  /** Blocks until at least `frames` have been rendered since start. */
  void waitForFrames(int64_t frames);

  /** Copy of everything rendered. Safe to call once stopped. */
  std::vector<float> captured() const;

  int64_t renderedFrames() const {
    return renderedFrames_.load(std::memory_order_acquire);
  }

 private:
  void run();

  const int32_t framesPerCallback_;
  IAudioRenderer* renderer_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<int64_t> renderedFrames_{0};
  mutable std::mutex captureMutex_;
  std::vector<float> capture_;
};

}  // namespace aidj::test
