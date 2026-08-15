#include "Mixer.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace aidj {

namespace {
/**
 * Ring capacity per voice. 4 seconds of stereo float at 48 kHz is ~1.5 MB and
 * gives the decoder thread a very wide margin against scheduler jitter without
 * pretending to hold a whole track in memory.
 */
constexpr size_t kRingSeconds = 4;
constexpr size_t kRingCapacitySamples =
    kRingSeconds * kEngineSampleRate * kEngineChannelCount;

/** Largest callback we will ever be handed, in frames. Scratch is sized to it. */
constexpr int32_t kMaxCallbackFrames = 8192;

constexpr float kPi = 3.14159265358979323846f;
}  // namespace

const char* toString(EngineError error) {
  switch (error) {
    case EngineError::None: return "NONE";
    case EngineError::OutputOpenFailed: return "OUTPUT_OPEN_FAILED";
    case EngineError::DecoderUnsupportedFormat: return "DECODER_UNSUPPORTED_FORMAT";
    case EngineError::DecoderIoError: return "DECODER_IO_ERROR";
    case EngineError::FileNotFound: return "FILE_NOT_FOUND";
    case EngineError::VoiceBusy: return "VOICE_BUSY";
    case EngineError::InvalidState: return "INVALID_STATE";
  }
  return "UNKNOWN";
}

bool CommandQueue::push(const Command& command) {
  const size_t writeIndex = writeIndex_.load(std::memory_order_relaxed);
  const size_t readIndex = readIndex_.load(std::memory_order_acquire);
  if (writeIndex - readIndex >= kCapacity - 1) {
    return false;  // Full. Caller decides; we never block the audio thread.
  }
  slots_[writeIndex & kMask] = command;
  writeIndex_.store(writeIndex + 1, std::memory_order_release);
  return true;
}

bool CommandQueue::pop(Command& out) {
  const size_t readIndex = readIndex_.load(std::memory_order_relaxed);
  const size_t writeIndex = writeIndex_.load(std::memory_order_acquire);
  if (readIndex == writeIndex) return false;
  out = slots_[readIndex & kMask];
  readIndex_.store(readIndex + 1, std::memory_order_release);
  return true;
}

float GainRamp::valueAt(int64_t frame) const {
  if (endFrame <= startFrame) return endGain;
  if (frame <= startFrame) return startGain;
  if (frame >= endFrame) return endGain;

  const float t = static_cast<float>(frame - startFrame) /
                  static_cast<float>(endFrame - startFrame);

  if (!equalPower) {
    return startGain + (endGain - startGain) * t;
  }

  // Equal power: a linear fade sums to a -3 dB dip in the middle when two
  // uncorrelated sources cross. Sine/cosine keeps summed power constant.
  const float shaped = (endGain >= startGain) ? std::sin(t * kPi * 0.5f)
                                              : std::cos(t * kPi * 0.5f);
  const float low = std::min(startGain, endGain);
  const float high = std::max(startGain, endGain);
  return low + (high - low) * shaped;
}

Mixer::Mixer() {
  for (auto& voice : voices_) {
    voice = std::make_unique<Voice>(kRingCapacitySamples);
  }
  for (auto& equaliser : equalisers_) {
    equaliser.configure(kEngineSampleRate);
  }
  scratch_.resize(static_cast<size_t>(kMaxCallbackFrames) * kEngineChannelCount,
                  0.0f);
}

bool Mixer::post(const Command& command) { return commands_.push(command); }

void Mixer::stageTransition(const TransitionSpec& spec, int32_t outgoingVoice,
                            int32_t incomingVoice) {
  stagedSpec_ = spec;
  stagedOutgoing_ = outgoingVoice;
  stagedIncoming_ = incomingVoice;
}

void Mixer::drainCommands() {
  Command command;
  const int64_t now = frameClock_.load(std::memory_order_relaxed);

  while (commands_.pop(command)) {
    switch (command.type) {
      case CommandType::SetVoiceActive: {
        voices_[static_cast<size_t>(command.voice)]->active.store(
            command.value > 0.5f, std::memory_order_relaxed);
        break;
      }
      case CommandType::SetVoiceGain: {
        auto& ramp = ramps_[static_cast<size_t>(command.voice)];
        ramp = GainRamp{now, now + command.durationFrames,
                        ramp.valueAt(now), command.value, false};
        break;
      }
      case CommandType::DevCrossfade: {
        const int64_t end = now + command.durationFrames;
        auto& outgoing = ramps_[static_cast<size_t>(command.voice)];
        auto& incoming = ramps_[static_cast<size_t>(command.voiceB)];
        outgoing = GainRamp{now, end, outgoing.valueAt(now), 0.0f, true};
        incoming = GainRamp{now, end, incoming.valueAt(now), 1.0f, true};
        voices_[static_cast<size_t>(command.voiceB)]->active.store(
            true, std::memory_order_relaxed);
        break;
      }
      case CommandType::ArmTransition: {
        // durationFrames carries the delay before the transition starts, so a
        // caller can arm it now to fire at a precise beat later.
        const int64_t start = now + command.durationFrames;
        timeline_.arm(stagedSpec_, start, kEngineSampleRate);
        outgoingVoice_ = stagedOutgoing_;
        incomingVoice_ = stagedIncoming_;

        // The incoming voice has to be running before the transition starts,
        // or its first bars are missing when the fade begins.
        if (incomingVoice_ >= 0) {
          voices_[static_cast<size_t>(incomingVoice_)]->active.store(
              true, std::memory_order_relaxed);
        }
        break;
      }
      case CommandType::ClearTransition: {
        timeline_.clear();
        outgoingVoice_ = -1;
        incomingVoice_ = -1;
        for (auto& equaliser : equalisers_) {
          equaliser.setBandGains(1.0f, 1.0f, 1.0f);
        }
        break;
      }
      case CommandType::Reset: {
        for (size_t i = 0; i < kVoiceCount; ++i) {
          ramps_[i] = GainRamp{};
          voices_[i]->active.store(false, std::memory_order_relaxed);
          equalisers_[i].setBandGains(1.0f, 1.0f, 1.0f);
          equalisers_[i].snapToTargets();
        }
        timeline_.clear();
        outgoingVoice_ = -1;
        incomingVoice_ = -1;
        break;
      }
      case CommandType::None:
        break;
    }
  }
}

/**
 * Soft-knee limiter. Transparent below the knee, asymptotic to the ceiling
 * above it, so a summed transition can never produce a hard-clipped edge.
 *
 * Known limitation: this has no lookahead, so it cannot catch an inter-sample
 * peak that only exists after reconstruction. That is acceptable while the
 * only thing being summed is two normalised voices; a proper lookahead limiter
 * belongs with the real transition engine.
 */
float Mixer::softLimit(float sample) {
  constexpr float knee = 0.7f;
  const float magnitude = std::fabs(sample);
  if (magnitude <= knee) return sample;

  const float sign = sample < 0.0f ? -1.0f : 1.0f;
  const float over = magnitude - knee;
  const float headroom = kOutputCeiling - knee;
  const float compressed = headroom * std::tanh(over / headroom);
  return sign * (knee + compressed);
}

void Mixer::render(float* output, int32_t frameCount) {
  drainCommands();

  const int32_t frames = std::min(frameCount, kMaxCallbackFrames);
  const size_t sampleCount = static_cast<size_t>(frames) * kEngineChannelCount;
  const int64_t blockStart = frameClock_.load(std::memory_order_relaxed);

  std::fill(output, output + sampleCount, 0.0f);

  for (size_t v = 0; v < kVoiceCount; ++v) {
    Voice& voice = *voices_[v];
    if (!voice.active.load(std::memory_order_relaxed)) {
      voice.currentGain.store(0.0f, std::memory_order_relaxed);
      continue;
    }

    // An incoming voice is activated when the transition is armed, which can
    // be many seconds before it starts, so that its decoder is running and its
    // ring is full when the fade begins. It must not be *consumed* during that
    // wait: reading the ring at zero gain silently plays the track, so by the
    // time the fade started it would be seconds past the cue point the planner
    // chose - the transition would land on the wrong bar, which is exactly
    // what a beat-matched mix cannot survive.
    if (timeline_.isArmed() && static_cast<int32_t>(v) == incomingVoice_ &&
        blockStart + frames <= timeline_.startFrame()) {
      voice.currentGain.store(0.0f, std::memory_order_relaxed);
      continue;
    }

    const size_t read = voice.ring.readOrSilence(scratch_.data(), sampleCount);
    if (read < sampleCount && !voice.endOfStream.load(std::memory_order_relaxed)) {
      starvedFrames_.fetch_add(
          static_cast<int64_t>((sampleCount - read) / kEngineChannelCount),
          std::memory_order_relaxed);
    }

    // A transition, when armed, overrides the plain gain ramp for the two
    // voices it involves. Everything else keeps using the ramp, so the
    // developer crossfade and manual gain still work unchanged.
    const bool inTransition =
        timeline_.isArmed() &&
        (static_cast<int32_t>(v) == outgoingVoice_ ||
         static_cast<int32_t>(v) == incomingVoice_);

    if (inTransition) {
      // EQ is applied to the whole block before the fade, using the values at
      // the block's midpoint. The EQ smooths internally per sample, so this
      // is not a stepped parameter - it is the target the smoother chases.
      const int64_t midpoint = blockStart + frames / 2;
      const VoiceParameters parameters =
          static_cast<int32_t>(v) == outgoingVoice_
              ? timeline_.outgoingAt(midpoint)
              : timeline_.incomingAt(midpoint);

      equalisers_[v].setBandGains(parameters.low, parameters.mid,
                                  parameters.high);
      equalisers_[v].process(scratch_.data(), static_cast<size_t>(frames));
    }

    const GainRamp& ramp = ramps_[v];
    float lastGain = 0.0f;
    for (int32_t f = 0; f < frames; ++f) {
      // Gain is evaluated per frame either way, so the fade itself stays
      // sample-accurate even though EQ targets update per block.
      const float gain =
          inTransition
              ? (static_cast<int32_t>(v) == outgoingVoice_
                     ? timeline_.outgoingAt(blockStart + f).gain
                     : timeline_.incomingAt(blockStart + f).gain)
              : ramp.valueAt(blockStart + f);
      lastGain = gain;
      const size_t base = static_cast<size_t>(f) * kEngineChannelCount;
      for (int32_t c = 0; c < kEngineChannelCount; ++c) {
        output[base + static_cast<size_t>(c)] +=
            scratch_[base + static_cast<size_t>(c)] * gain;
      }
    }

    voice.currentGain.store(lastGain, std::memory_order_relaxed);
    voice.framesRendered.fetch_add(
        static_cast<int64_t>(read / kEngineChannelCount),
        std::memory_order_relaxed);
  }

  // Retire a finished transition. The outgoing voice is deactivated rather
  // than left running silently: a voice at zero gain still drains its ring and
  // holds a decoder, and the preparation pipeline needs that slot back for the
  // track after next.
  if (timeline_.isArmed() && timeline_.isComplete(blockStart + frames)) {
    if (outgoingVoice_ >= 0) {
      voices_[static_cast<size_t>(outgoingVoice_)]->active.store(
          false, std::memory_order_relaxed);
      equalisers_[static_cast<size_t>(outgoingVoice_)].setBandGains(1.0f, 1.0f,
                                                                   1.0f);
    }
    if (incomingVoice_ >= 0) {
      // The incoming track is now simply playing, at its loudness-matched
      // level, so hand it back to the ordinary gain ramp.
      ramps_[static_cast<size_t>(incomingVoice_)] =
          GainRamp{0, 0, timeline_.incomingAt(blockStart + frames).gain,
                   timeline_.incomingAt(blockStart + frames).gain, false};
    }
    timeline_.clear();
    outgoingVoice_ = -1;
    incomingVoice_ = -1;
    transitionsCompleted_.fetch_add(1, std::memory_order_relaxed);
  }

  for (size_t i = 0; i < sampleCount; ++i) {
    output[i] = softLimit(output[i]);
  }

  frameClock_.store(blockStart + frames, std::memory_order_relaxed);
}

}  // namespace aidj
