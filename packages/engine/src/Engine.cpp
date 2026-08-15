#include "Engine.h"

#include <utility>

namespace aidj {

namespace {
/** Gain changes shorter than this click. Used for play/stop, not transitions. */
constexpr int32_t kDefaultGainRampMs = 20;

int32_t msToFrames(int32_t ms) { return ms * kEngineSampleRate / 1000; }
}  // namespace

Engine::Engine() = default;

Engine::~Engine() { shutdown(); }

EngineError Engine::initialise(std::unique_ptr<IAudioOutput> output) {
  std::lock_guard<std::mutex> lock(controlMutex_);
  if (output_ != nullptr) return EngineError::None;

  output_ = std::move(output);
  if (!output_->start(&mixer_)) {
    output_.reset();
    lastError_.store(EngineError::OutputOpenFailed, std::memory_order_relaxed);
    setState(EngineState::Error);
    return EngineError::OutputOpenFailed;
  }

  setState(EngineState::Idle);
  return EngineError::None;
}

void Engine::shutdown() {
  std::lock_guard<std::mutex> lock(controlMutex_);
  for (auto& pump : pumps_) {
    if (pump != nullptr) pump->stop();
  }
  pumps_ = {};
  voiceLoaded_ = {};
  if (output_ != nullptr) {
    output_->stop();
    output_.reset();
  }
  setState(EngineState::Uninitialised);
}

EngineError Engine::loadVoice(int32_t voiceIndex, const std::string& uri) {
  std::lock_guard<std::mutex> lock(controlMutex_);
  if (!isValidVoice(voiceIndex)) return EngineError::InvalidState;
  if (output_ == nullptr) return EngineError::InvalidState;

  const size_t index = static_cast<size_t>(voiceIndex);

  // Silence and detach before touching the ring: the audio thread must never
  // read a voice whose buffer is being reset underneath it.
  mixer_.post(Command{CommandType::SetVoiceActive, voiceIndex, 0, 0.0f, 0});
  if (pumps_[index] != nullptr) pumps_[index]->stop();

  auto decoder = createPlatformDecoder();
  if (decoder == nullptr) return EngineError::DecoderUnsupportedFormat;

  auto pump = std::make_unique<DecodePump>(mixer_.voice(voiceIndex),
                                           std::move(decoder));
  const EngineError error = pump->start(uri);
  if (error != EngineError::None) {
    voiceLoaded_[index] = false;
    lastError_.store(error, std::memory_order_relaxed);
    return error;
  }

  pumps_[index] = std::move(pump);
  voiceLoaded_[index] = true;
  if (state_.load(std::memory_order_acquire) == EngineState::Idle) {
    setState(EngineState::Preparing);
  }
  return EngineError::None;
}

EngineError Engine::prepareVoice(int32_t voiceIndex, const std::string& uri,
                                 double startMs, double tempoRatio) {
  {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (!isValidVoice(voiceIndex)) return EngineError::InvalidState;
    if (output_ == nullptr) return EngineError::InvalidState;

    const size_t index = static_cast<size_t>(voiceIndex);
    mixer_.post(Command{CommandType::SetVoiceActive, voiceIndex, 0, 0.0f, 0});
    if (pumps_[index] != nullptr) pumps_[index]->stop();

    auto decoder = createPlatformDecoder();
    if (decoder == nullptr) return EngineError::DecoderUnsupportedFormat;

    auto pump = std::make_unique<DecodePump>(mixer_.voice(voiceIndex),
                                             std::move(decoder));
    // The ratio must be set before the thread starts, or the first chunk of
    // audio reaches the ring at the wrong tempo and the transition begins with
    // the two grids already apart.
    pump->setTempoRatio(tempoRatio);

    const EngineError error = pump->start(uri, startMs);
    if (error != EngineError::None) {
      voiceLoaded_[index] = false;
      lastError_.store(error, std::memory_order_relaxed);
      return error;
    }

    pumps_[index] = std::move(pump);
    voiceLoaded_[index] = true;
  }
  return EngineError::None;
}

EngineError Engine::armTransition(const TransitionSpec& spec,
                                  int32_t outgoingVoice, int32_t incomingVoice,
                                  double delayMs) {
  std::lock_guard<std::mutex> lock(controlMutex_);
  if (!isValidVoice(outgoingVoice) || !isValidVoice(incomingVoice)) {
    return EngineError::InvalidState;
  }
  if (outgoingVoice == incomingVoice) return EngineError::InvalidState;
  if (!voiceLoaded_[static_cast<size_t>(incomingVoice)]) {
    return EngineError::InvalidState;
  }

  mixer_.stageTransition(spec, outgoingVoice, incomingVoice);
  mixer_.post(Command{CommandType::ArmTransition, outgoingVoice, incomingVoice,
                      0.0f, msToFrames(static_cast<int32_t>(delayMs))});
  setState(EngineState::Playing);
  return EngineError::None;
}

void Engine::clearTransition() {
  std::lock_guard<std::mutex> lock(controlMutex_);
  mixer_.post(Command{CommandType::ClearTransition, 0, 0, 0.0f, 0});
}

EngineError Engine::playVoice(int32_t voiceIndex) {
  std::lock_guard<std::mutex> lock(controlMutex_);
  if (!isValidVoice(voiceIndex)) return EngineError::InvalidState;
  const size_t index = static_cast<size_t>(voiceIndex);
  if (!voiceLoaded_[index]) return EngineError::InvalidState;

  mixer_.post(Command{CommandType::SetVoiceActive, voiceIndex, 0, 1.0f, 0});
  mixer_.post(Command{CommandType::SetVoiceGain, voiceIndex, 0, 1.0f,
                      msToFrames(kDefaultGainRampMs)});
  setState(EngineState::Playing);
  return EngineError::None;
}

EngineError Engine::pause() {
  std::lock_guard<std::mutex> lock(controlMutex_);
  if (state_.load(std::memory_order_acquire) != EngineState::Playing) {
    return EngineError::InvalidState;
  }
  // Stopping the output stream rather than zeroing gain: it releases the audio
  // hardware, which matters for battery, and it keeps ring contents intact so
  // resume does not have to re-decode.
  if (output_ != nullptr) output_->stop();
  setState(EngineState::Paused);
  return EngineError::None;
}

EngineError Engine::resume() {
  std::lock_guard<std::mutex> lock(controlMutex_);
  if (state_.load(std::memory_order_acquire) != EngineState::Paused) {
    return EngineError::InvalidState;
  }
  if (output_ == nullptr || !output_->start(&mixer_)) {
    lastError_.store(EngineError::OutputOpenFailed, std::memory_order_relaxed);
    setState(EngineState::Error);
    return EngineError::OutputOpenFailed;
  }
  setState(EngineState::Playing);
  return EngineError::None;
}

void Engine::stopAll() {
  std::lock_guard<std::mutex> lock(controlMutex_);
  mixer_.post(Command{CommandType::Reset, 0, 0, 0.0f, 0});
  for (auto& pump : pumps_) {
    if (pump != nullptr) pump->stop();
  }
  pumps_ = {};
  voiceLoaded_ = {};
  if (output_ != nullptr) setState(EngineState::Idle);
}

EngineError Engine::devCrossfade(int32_t fromVoice, int32_t toVoice,
                                 int32_t durationMs) {
  std::lock_guard<std::mutex> lock(controlMutex_);
  if (!isValidVoice(fromVoice) || !isValidVoice(toVoice)) {
    return EngineError::InvalidState;
  }
  if (fromVoice == toVoice) return EngineError::InvalidState;
  if (!voiceLoaded_[static_cast<size_t>(fromVoice)] ||
      !voiceLoaded_[static_cast<size_t>(toVoice)]) {
    return EngineError::InvalidState;
  }
  if (durationMs < 100 || durationMs > 60000) return EngineError::InvalidState;

  mixer_.post(Command{CommandType::DevCrossfade, fromVoice, toVoice, 0.0f,
                      msToFrames(durationMs)});
  return EngineError::None;
}

EngineStatus Engine::status() const {
  EngineStatus result;
  result.state = state_.load(std::memory_order_acquire);
  result.lastError = lastError_.load(std::memory_order_relaxed);
  result.starvedFrames = mixer_.starvedFrames();

  if (output_ != nullptr) {
    result.underrunCount = output_->underrunCount();
    result.framesPerBurst = output_->framesPerBurst();
    result.outputLatencyMs = output_->outputLatencyMs();
  }

  for (size_t i = 0; i < kVoiceCount; ++i) {
    const Voice& voice = mixer_.voice(static_cast<int32_t>(i));
    VoiceStatus& out = result.voices[i];
    out.hasSource = voiceLoaded_[i];
    out.positionMs = voice.framesRendered.load(std::memory_order_relaxed) *
                     1000 / kEngineSampleRate;
    out.durationMs = voice.durationFrames.load(std::memory_order_relaxed) *
                     1000 / kEngineSampleRate;
    out.gain = voice.currentGain.load(std::memory_order_relaxed);
    out.primed = voice.primed.load(std::memory_order_acquire);
    out.endOfStream = voice.endOfStream.load(std::memory_order_acquire);
  }
  return result;
}

}  // namespace aidj
