#include "TestDecoder.h"

#include <cmath>
#include <cstdlib>
#include <memory>

namespace aidj::test {

namespace {
constexpr double kTwoPi = 6.283185307179586;

int queryInt(const std::string& uri, const std::string& key, int fallback) {
  const std::string needle = key + "=";
  const size_t at = uri.find(needle);
  if (at == std::string::npos) return fallback;
  return std::atoi(uri.c_str() + at + needle.size());
}
}  // namespace

std::atomic<int>& TestDecoder::openCount() {
  static std::atomic<int> count{0};
  return count;
}

EngineError TestDecoder::open(const std::string& uri) {
  close();

  if (uri.rfind("test://", 0) != 0) return EngineError::FileNotFound;
  if (uri.find("unsupported") != std::string::npos) {
    return EngineError::DecoderUnsupportedFormat;
  }

  frequencyHz_ = queryInt(uri, "hz", 440);
  format_.sampleRate = queryInt(uri, "rate", 48000);
  format_.channelCount = queryInt(uri, "ch", 2);
  format_.durationMs = queryInt(uri, "ms", 1000);
  totalFrames_ = format_.durationMs * format_.sampleRate / 1000;
  framesProduced_ = 0;
  open_ = true;
  openCount().fetch_add(1);
  return EngineError::None;
}

size_t TestDecoder::decode(float* destination, size_t maxSamples) {
  if (!open_) return 0;

  const size_t channels = static_cast<size_t>(format_.channelCount);
  const size_t framesWanted = maxSamples / channels;
  size_t framesWritten = 0;

  while (framesWritten < framesWanted && framesProduced_ < totalFrames_) {
    const double phase = kTwoPi * frequencyHz_ *
                         static_cast<double>(framesProduced_) /
                         static_cast<double>(format_.sampleRate);
    const auto value = static_cast<float>(std::sin(phase)) * 0.5f;
    for (size_t c = 0; c < channels; ++c) {
      destination[framesWritten * channels + c] = value;
    }
    ++framesWritten;
    ++framesProduced_;
  }

  return framesWritten * channels;
}

void TestDecoder::close() {
  if (open_) openCount().fetch_sub(1);
  open_ = false;
  framesProduced_ = 0;
}

}  // namespace aidj::test

namespace aidj {
/**
 * The host build has no platform decoder, so the test double provides the
 * symbol. On Android this is defined by MediaCodecDecoder.cpp instead.
 */
std::unique_ptr<IDecoder> createPlatformDecoder() {
  return std::make_unique<test::TestDecoder>();
}
}  // namespace aidj
