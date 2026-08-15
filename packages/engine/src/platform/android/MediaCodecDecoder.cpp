#include "MediaCodecDecoder.h"

#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#define LOG_TAG "AiDjDecoder"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aidj {

namespace {
/** Values of AMEDIAFORMAT_KEY_PCM_ENCODING. */
constexpr int32_t kPcmEncoding16Bit = 2;
constexpr int32_t kPcmEncodingFloat = 4;

// AMEDIAFORMAT_KEY_PCM_ENCODING is __INTRODUCED_IN(28) and we ship minSdk 26,
// so referencing the exported symbol would not link on API 26/27. The key is
// just this string, and AMediaFormat_getInt32 returns false when a format has
// no such entry - which is exactly the pre-28 behaviour we want, falling back
// to the 16-bit assumption below.
constexpr const char* kKeyPcmEncoding = "pcm-encoding";

constexpr int64_t kDequeueTimeoutUs = 10000;  // 10 ms

bool startsWith(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}
}  // namespace

MediaCodecDecoder::~MediaCodecDecoder() { close(); }

EngineError MediaCodecDecoder::open(const std::string& uri) {
  close();

  extractor_ = AMediaExtractor_new();
  if (extractor_ == nullptr) return EngineError::DecoderIoError;

  media_status_t status;
  if (startsWith(uri, "fd://")) {
    // A descriptor the Java layer already resolved for us. content:// URIs
    // cannot be opened here: AMediaExtractor_setDataSource has no access to
    // ContentResolver, which is the only thing that can resolve a SAF grant.
    // The caller hands ownership of the descriptor over, so we always close it.
    int fd = -1;
    long long offset = 0;
    long long length = 0;
    if (std::sscanf(uri.c_str(), "fd://%d?offset=%lld&length=%lld", &fd, &offset,
                    &length) != 3 ||
        fd < 0) {
      close();
      return EngineError::DecoderIoError;
    }
    status = AMediaExtractor_setDataSourceFd(extractor_, fd,
                                             static_cast<off64_t>(offset),
                                             static_cast<off64_t>(length));
    ::close(fd);  // The extractor dups the descriptor.
  } else if (startsWith(uri, "http://") || startsWith(uri, "https://")) {
    status = AMediaExtractor_setDataSource(extractor_, uri.c_str());
  } else {
    // A plain path, or a file:// URI. Open a descriptor ourselves so a missing
    // file is reported as FILE_NOT_FOUND rather than a generic extractor error.
    std::string path = uri;
    if (startsWith(path, "file://")) path = path.substr(7);

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      close();
      return EngineError::FileNotFound;
    }
    const off_t length = ::lseek(fd, 0, SEEK_END);
    ::lseek(fd, 0, SEEK_SET);
    status = AMediaExtractor_setDataSourceFd(extractor_, fd, 0,
                                             static_cast<off64_t>(length));
    ::close(fd);  // The extractor dups the descriptor.
  }

  if (status != AMEDIA_OK) {
    close();
    return EngineError::DecoderIoError;
  }

  const EngineError trackError = selectAudioTrack();
  if (trackError != EngineError::None) {
    close();
    return trackError;
  }
  return EngineError::None;
}

EngineError MediaCodecDecoder::selectAudioTrack() {
  const size_t trackCount = AMediaExtractor_getTrackCount(extractor_);

  for (size_t i = 0; i < trackCount; ++i) {
    AMediaFormat* trackFormat = AMediaExtractor_getTrackFormat(extractor_, i);
    if (trackFormat == nullptr) continue;

    const char* mime = nullptr;
    if (!AMediaFormat_getString(trackFormat, AMEDIAFORMAT_KEY_MIME, &mime) ||
        mime == nullptr || std::strncmp(mime, "audio/", 6) != 0) {
      AMediaFormat_delete(trackFormat);
      continue;
    }

    if (AMediaExtractor_selectTrack(extractor_, i) != AMEDIA_OK) {
      AMediaFormat_delete(trackFormat);
      continue;
    }

    codec_ = AMediaCodec_createDecoderByType(mime);
    if (codec_ == nullptr) {
      AMediaFormat_delete(trackFormat);
      return EngineError::DecoderUnsupportedFormat;
    }

    if (AMediaCodec_configure(codec_, trackFormat, nullptr, nullptr, 0) !=
            AMEDIA_OK ||
        AMediaCodec_start(codec_) != AMEDIA_OK) {
      AMediaFormat_delete(trackFormat);
      return EngineError::DecoderUnsupportedFormat;
    }

    int32_t sampleRate = 0;
    int32_t channelCount = 0;
    int64_t durationUs = 0;
    AMediaFormat_getInt32(trackFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sampleRate);
    AMediaFormat_getInt32(trackFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT,
                          &channelCount);
    AMediaFormat_getInt64(trackFormat, AMEDIAFORMAT_KEY_DURATION, &durationUs);

    format_.sampleRate = sampleRate;
    format_.channelCount = channelCount;
    format_.durationMs = durationUs / 1000;

    AMediaFormat_delete(trackFormat);
    return EngineError::None;
  }

  return EngineError::DecoderUnsupportedFormat;
}

DecodedFormat MediaCodecDecoder::format() const { return format_; }

bool MediaCodecDecoder::pumpCodec() {
  if (codec_ == nullptr) return false;

  // Feed the codec whatever the extractor has, one access unit at a time.
  if (!inputExhausted_) {
    const ssize_t inputIndex =
        AMediaCodec_dequeueInputBuffer(codec_, kDequeueTimeoutUs);
    if (inputIndex >= 0) {
      size_t bufferSize = 0;
      uint8_t* buffer = AMediaCodec_getInputBuffer(
          codec_, static_cast<size_t>(inputIndex), &bufferSize);

      const ssize_t sampleSize =
          buffer == nullptr
              ? -1
              : AMediaExtractor_readSampleData(extractor_, buffer, bufferSize);

      if (sampleSize <= 0) {
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(inputIndex), 0,
                                     0, 0,
                                     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        inputExhausted_ = true;
      } else {
        const int64_t presentationTimeUs =
            AMediaExtractor_getSampleTime(extractor_);
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(inputIndex), 0,
                                     static_cast<size_t>(sampleSize),
                                     presentationTimeUs, 0);
        AMediaExtractor_advance(extractor_);
      }
    }
  }

  AMediaCodecBufferInfo info{};
  const ssize_t outputIndex =
      AMediaCodec_dequeueOutputBuffer(codec_, &info, kDequeueTimeoutUs);

  if (outputIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
    // The real PCM encoding is only knowable from the *output* format, and it
    // differs across devices - some hand back float, most hand back 16-bit.
    AMediaFormat* outputFormat = AMediaCodec_getOutputFormat(codec_);
    if (outputFormat != nullptr) {
      int32_t encoding = kPcmEncoding16Bit;
      if (AMediaFormat_getInt32(outputFormat, kKeyPcmEncoding, &encoding)) {
        pcmEncoding_ = encoding;
      }
      int32_t sampleRate = 0;
      int32_t channelCount = 0;
      if (AMediaFormat_getInt32(outputFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE,
                                &sampleRate) &&
          sampleRate > 0) {
        format_.sampleRate = sampleRate;
      }
      if (AMediaFormat_getInt32(outputFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT,
                                &channelCount) &&
          channelCount > 0) {
        format_.channelCount = channelCount;
      }
      AMediaFormat_delete(outputFormat);
    }
    return true;
  }

  if (outputIndex < 0) {
    // TRY_AGAIN_LATER or BUFFERS_CHANGED - both mean "no output this round".
    return !outputExhausted_;
  }

  size_t outputSize = 0;
  uint8_t* outputBuffer = AMediaCodec_getOutputBuffer(
      codec_, static_cast<size_t>(outputIndex), &outputSize);

  if (outputBuffer != nullptr && info.size > 0) {
    const uint8_t* start = outputBuffer + info.offset;
    if (pcmEncoding_ == kPcmEncodingFloat) {
      const auto* samples = reinterpret_cast<const float*>(start);
      const size_t count = static_cast<size_t>(info.size) / sizeof(float);
      pending_.insert(pending_.end(), samples, samples + count);
    } else {
      const auto* samples = reinterpret_cast<const int16_t*>(start);
      const size_t count = static_cast<size_t>(info.size) / sizeof(int16_t);
      pending_.reserve(pending_.size() + count);
      for (size_t i = 0; i < count; ++i) {
        // Divide by 32768 so full-scale negative maps to exactly -1.0 and no
        // value can exceed +/-1.0 - dividing by 32767 would clip on peaks.
        pending_.push_back(static_cast<float>(samples[i]) / 32768.0f);
      }
    }
  }

  AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(outputIndex),
                                  false);

  if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
    outputExhausted_ = true;
    return false;
  }
  return true;
}

size_t MediaCodecDecoder::decode(float* destination, size_t maxSamples) {
  if (codec_ == nullptr) return 0;

  while (pending_.size() - pendingOffset_ < maxSamples && !outputExhausted_) {
    if (!pumpCodec()) break;
  }

  const size_t available = pending_.size() - pendingOffset_;
  const size_t toCopy = std::min(available, maxSamples);
  if (toCopy > 0) {
    std::memcpy(destination, pending_.data() + pendingOffset_,
                toCopy * sizeof(float));
    pendingOffset_ += toCopy;
  }

  // Compact once the consumed prefix dominates, so a long track does not grow
  // the buffer without bound.
  if (pendingOffset_ > 0 && pendingOffset_ * 2 >= pending_.size()) {
    pending_.erase(pending_.begin(),
                   pending_.begin() + static_cast<long>(pendingOffset_));
    pendingOffset_ = 0;
  }

  return toCopy;
}

void MediaCodecDecoder::close() {
  if (codec_ != nullptr) {
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
  }
  if (extractor_ != nullptr) {
    AMediaExtractor_delete(extractor_);
    extractor_ = nullptr;
  }
  pending_.clear();
  pendingOffset_ = 0;
  inputExhausted_ = false;
  outputExhausted_ = false;
  format_ = DecodedFormat{};
}

EngineError MediaCodecDecoder::seek(double positionMs) {
  if (extractor_ == nullptr || codec_ == nullptr) {
    return EngineError::InvalidState;
  }

  // Seek to the closest sync sample at or before the target. Landing on a sync
  // point matters: starting mid-frame gives a burst of garbage before the
  // decoder resynchronises, which at a cue point is the first thing heard.
  const int64_t positionUs = static_cast<int64_t>(positionMs * 1000.0);
  if (AMediaExtractor_seekTo(extractor_, positionUs,
                             AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC) != AMEDIA_OK) {
    return EngineError::DecoderIoError;
  }

  // The codec is holding frames from before the seek; flushing discards them
  // so the next output belongs to the new position.
  AMediaCodec_flush(codec_);

  pending_.clear();
  pendingOffset_ = 0;
  inputExhausted_ = false;
  outputExhausted_ = false;
  return EngineError::None;
}

std::unique_ptr<IDecoder> createPlatformDecoder() {
  return std::make_unique<MediaCodecDecoder>();
}

}  // namespace aidj
