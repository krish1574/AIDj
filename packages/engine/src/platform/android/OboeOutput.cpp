#include "OboeOutput.h"

#include <android/log.h>

#include "../../AudioTypes.h"

#define LOG_TAG "AiDjOboe"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace aidj {

OboeOutput::~OboeOutput() { stop(); }

bool OboeOutput::start(IAudioRenderer* renderer) {
  if (stream_ != nullptr) return true;
  renderer_.store(renderer, std::memory_order_release);

  oboe::AudioStreamBuilder builder;
  builder.setDirection(oboe::Direction::Output)
      ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
      ->setSharingMode(oboe::SharingMode::Exclusive)
      ->setFormat(oboe::AudioFormat::Float)
      ->setChannelCount(kEngineChannelCount)
      ->setSampleRate(kEngineSampleRate)
      // Let Oboe resample if the device will not give us 48 kHz natively;
      // its converter is better than anything we would write for this.
      ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
      ->setUsage(oboe::Usage::Media)
      ->setContentType(oboe::ContentType::Music)
      ->setDataCallback(this)
      ->setErrorCallback(this);

  const oboe::Result result = builder.openStream(stream_);
  if (result != oboe::Result::OK) {
    LOGE("openStream failed: %s", oboe::convertToText(result));
    stream_.reset();
    return false;
  }

  // Two bursts is the standard low-latency compromise: enough slack for
  // scheduler jitter, small enough to keep latency usable.
  stream_->setBufferSizeInFrames(stream_->getFramesPerBurst() * 2);

  const oboe::Result startResult = stream_->requestStart();
  if (startResult != oboe::Result::OK) {
    LOGE("requestStart failed: %s", oboe::convertToText(startResult));
    stream_->close();
    stream_.reset();
    return false;
  }

  LOGI("stream open: rate=%d burst=%d api=%s", stream_->getSampleRate(),
       stream_->getFramesPerBurst(),
       oboe::convertToText(stream_->getAudioApi()));
  return true;
}

void OboeOutput::stop() {
  if (stream_ == nullptr) return;
  stream_->requestStop();
  stream_->close();
  stream_.reset();
  renderer_.store(nullptr, std::memory_order_release);
}

oboe::DataCallbackResult OboeOutput::onAudioReady(oboe::AudioStream* /*stream*/,
                                                  void* audioData,
                                                  int32_t numFrames) {
  IAudioRenderer* renderer = renderer_.load(std::memory_order_acquire);
  auto* out = static_cast<float*>(audioData);

  if (renderer == nullptr) {
    const size_t samples =
        static_cast<size_t>(numFrames) * kEngineChannelCount;
    for (size_t i = 0; i < samples; ++i) out[i] = 0.0f;
    return oboe::DataCallbackResult::Continue;
  }

  renderer->render(out, numFrames);
  return oboe::DataCallbackResult::Continue;
}

void OboeOutput::onErrorAfterClose(oboe::AudioStream* /*stream*/,
                                   oboe::Result error) {
  // Happens on device changes - headphones unplugged, Bluetooth connected.
  // Milestone 1 records it; automatic re-open belongs with the continuous
  // player in Milestone 6, where there is a session to restore into.
  LOGE("stream closed with error: %s", oboe::convertToText(error));
  restartNeeded_.store(true, std::memory_order_release);
}

int32_t OboeOutput::underrunCount() const {
  if (stream_ == nullptr) return 0;
  const auto result = stream_->getXRunCount();
  return result ? result.value() : 0;
}

int32_t OboeOutput::framesPerBurst() const {
  return stream_ == nullptr ? 0 : stream_->getFramesPerBurst();
}

double OboeOutput::outputLatencyMs() const {
  if (stream_ == nullptr) return -1.0;
  const auto latency = stream_->calculateLatencyMillis();
  return latency ? latency.value() : -1.0;
}

}  // namespace aidj
