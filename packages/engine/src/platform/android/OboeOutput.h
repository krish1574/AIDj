#pragma once

#include <oboe/Oboe.h>

#include <atomic>
#include <memory>

#include "../../IAudioOutput.h"

namespace aidj {

/**
 * Oboe-backed output.
 *
 * Requests the exclusive, low-latency AAudio path with PerformanceMode::
 * LowLatency; Oboe falls back to OpenSL ES on older devices without us
 * branching. The callback does nothing but forward to the mixer - all
 * scheduling decisions belong upstream.
 */
class OboeOutput final : public IAudioOutput,
                         public oboe::AudioStreamDataCallback,
                         public oboe::AudioStreamErrorCallback {
 public:
  ~OboeOutput() override;

  bool start(IAudioRenderer* renderer) override;
  void stop() override;

  int32_t underrunCount() const override;
  int32_t framesPerBurst() const override;
  double outputLatencyMs() const override;

  oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData,
                                        int32_t numFrames) override;

  void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

 private:
  std::shared_ptr<oboe::AudioStream> stream_;
  std::atomic<IAudioRenderer*> renderer_{nullptr};
  /** Set when the device tears the stream down, e.g. on a headphone unplug. */
  std::atomic<bool> restartNeeded_{false};
};

}  // namespace aidj
