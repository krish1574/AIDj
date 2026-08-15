#pragma once

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../../IDecoder.h"

namespace aidj {

/**
 * Hardware-accelerated decoding through the NDK media APIs.
 *
 * Chosen over FFmpeg deliberately: no GPL/LGPL relinking obligation, no extra
 * ~10 MB per ABI in the APK, and the codecs are the device's own, so decoding a
 * full playlist ahead of playback costs far less battery than a software
 * decoder would. It covers mp3, aac/m4a, flac, wav, ogg and opus - the whole
 * realistic local-library set.
 *
 * Outputs interleaved float at the source's own rate; resampling happens a
 * layer up in DecodePump.
 */
class MediaCodecDecoder final : public IDecoder {
 public:
  ~MediaCodecDecoder() override;

  EngineError open(const std::string& uri) override;
  DecodedFormat format() const override;
  size_t decode(float* destination, size_t maxSamples) override;
  void close() override;

 private:
  EngineError selectAudioTrack();
  /** Pulls one output buffer from the codec into `pending_`. */
  bool pumpCodec();

  AMediaExtractor* extractor_ = nullptr;
  AMediaCodec* codec_ = nullptr;
  DecodedFormat format_{};
  /** PCM encoding reported by the codec: 16-bit integer or float. */
  int32_t pcmEncoding_ = 2;  // AMEDIAFORMAT_PCM_ENCODING value for 16-bit
  bool inputExhausted_ = false;
  bool outputExhausted_ = false;
  /** Converted float samples not yet handed to the caller. */
  std::vector<float> pending_;
  size_t pendingOffset_ = 0;
};

}  // namespace aidj
