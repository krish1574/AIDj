#include "NullOutput.h"

#include <chrono>

#include "AudioTypes.h"

namespace aidj::test {

NullOutput::NullOutput(int32_t framesPerCallback)
    : framesPerCallback_(framesPerCallback) {}

NullOutput::~NullOutput() { stop(); }

bool NullOutput::start(IAudioRenderer* renderer) {
  if (running_.load(std::memory_order_acquire)) return true;
  renderer_ = renderer;
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this] { run(); });
  return true;
}

void NullOutput::stop() {
  running_.store(false, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
  renderer_ = nullptr;
}

void NullOutput::run() {
  std::vector<float> buffer(
      static_cast<size_t>(framesPerCallback_) * kEngineChannelCount, 0.0f);

  while (running_.load(std::memory_order_acquire)) {
    renderer_->render(buffer.data(), framesPerCallback_);
    {
      std::lock_guard<std::mutex> lock(captureMutex_);
      capture_.insert(capture_.end(), buffer.begin(), buffer.end());
    }
    renderedFrames_.fetch_add(framesPerCallback_, std::memory_order_release);
    // Yield rather than sleep: fast enough that tests finish quickly, but not
    // a busy spin that starves the decoder thread we are testing against.
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

void NullOutput::waitForFrames(int64_t frames) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (renderedFrames_.load(std::memory_order_acquire) < frames) {
    if (std::chrono::steady_clock::now() > deadline) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::vector<float> NullOutput::captured() const {
  std::lock_guard<std::mutex> lock(captureMutex_);
  return capture_;
}

}  // namespace aidj::test
