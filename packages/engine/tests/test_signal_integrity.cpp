#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "analysis/Fft.h"
#include "dsp/Resampler.h"
#include "dsp/ThreeBandEq.h"
#include "dsp/TimeStretch.h"

using aidj::Resampler;
using aidj::ThreeBandEq;
using aidj::TimeStretch;

namespace {

constexpr int32_t kSampleRate = 48000;
constexpr double kPi = 3.14159265358979323846;

/**
 * Music-like test signal: a harmonic stack over a bass note, plus periodic
 * transients.
 *
 * Every earlier DSP test in this suite used a single sine, which is the most
 * forgiving signal there is - it is periodic everywhere, so splice and phase
 * errors that would wreck real music are invisible. This exists because the
 * engine sounded broken on real audio while every sine test passed.
 */
std::vector<float> musicLike(size_t frames, int32_t sampleRate = kSampleRate) {
  std::vector<float> out(frames * 2, 0.0f);

  const double fundamental = 110.0;  // A2
  const double harmonics[] = {1.0, 2.0, 3.0, 4.0, 5.0, 7.0};
  const double weights[] = {1.0, 0.6, 0.4, 0.25, 0.15, 0.1};

  for (size_t f = 0; f < frames; ++f) {
    const double t = static_cast<double>(f) / sampleRate;
    double sample = 0.0;
    for (int h = 0; h < 6; ++h) {
      sample += weights[h] * std::sin(2.0 * kPi * fundamental * harmonics[h] * t);
    }
    sample *= 0.18;

    // A transient every 500 ms, which is what smears if splicing is wrong.
    const size_t sinceBeat = f % static_cast<size_t>(sampleRate / 2);
    if (sinceBeat < 200) {
      const double decay = std::exp(-static_cast<double>(sinceBeat) / 40.0);
      sample += 0.35 * decay * std::sin(2.0 * kPi * 60.0 * t);
    }

    out[f * 2] = static_cast<float>(sample);
    out[f * 2 + 1] = static_cast<float>(sample);
  }
  return out;
}

/** Ratio of energy outside the source's harmonics to energy inside them. */
double spuriousEnergyRatio(const std::vector<float>& interleaved,
                           size_t startFrame) {
  constexpr size_t kFftSize = 16384;
  const size_t frames = interleaved.size() / 2;
  REQUIRE(frames > startFrame + kFftSize);

  aidj::Fft fft(kFftSize);
  const std::vector<float> window = aidj::hannWindow(kFftSize);
  std::vector<float> frame(kFftSize);
  for (size_t i = 0; i < kFftSize; ++i) {
    frame[i] = interleaved[(startFrame + i) * 2] * window[i];
  }

  std::vector<float> magnitudes(kFftSize / 2 + 1, 0.0f);
  fft.magnitudeSpectrum(frame.data(), magnitudes.data());

  // Bins belonging to the fundamental and its harmonics, with a skirt.
  const double binHz = static_cast<double>(kSampleRate) / kFftSize;
  double signal = 0.0;
  double spurious = 0.0;

  for (size_t bin = 1; bin < magnitudes.size(); ++bin) {
    const double hz = static_cast<double>(bin) * binHz;
    bool isHarmonic = false;
    for (int h = 1; h <= 10; ++h) {
      if (std::abs(hz - 110.0 * h) < 25.0) isHarmonic = true;
    }
    // The transient is broadband and low; exclude it from the judgement.
    if (hz < 90.0) continue;

    const double power = static_cast<double>(magnitudes[bin]) * magnitudes[bin];
    if (isHarmonic) signal += power;
    else spurious += power;
  }

  return signal > 0.0 ? spurious / signal : 1.0;
}

float peakOf(const std::vector<float>& samples) {
  float result = 0.0f;
  for (float sample : samples) result = std::max(result, std::abs(sample));
  return result;
}

}  // namespace

TEST_CASE("resampler keeps complex material clean", "[integrity]") {
  // 44.1 -> 48 kHz is what nearly every real file goes through.
  Resampler resampler;
  resampler.configure(44100, 2, kSampleRate);

  const auto input = musicLike(44100 * 4, 44100);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);
  resampler.finish(output);

  const double ratio = spuriousEnergyRatio(output, kSampleRate);
  const double decibels = 10.0 * std::log10(ratio + 1e-20);
  INFO("resampler spurious/signal: " << decibels << " dB");

  REQUIRE(peakOf(output) > 0.1f);
  REQUIRE(decibels < -40.0);
}

TEST_CASE("time stretch keeps complex material intelligible", "[integrity]") {
  // The stage most likely to wreck real audio: WSOLA splices waveform
  // segments, and a splice at the wrong phase is audible as roughness on
  // harmonic material even when a sine test passes perfectly.
  TimeStretch stretch;
  stretch.configure(kSampleRate, 2);
  stretch.setRatio(1.03);

  const auto input = musicLike(kSampleRate * 4);
  std::vector<float> output;
  stretch.process(input.data(), input.size() / 2, output);
  stretch.finish(output);

  const double ratio = spuriousEnergyRatio(output, kSampleRate);
  const double decibels = 10.0 * std::log10(ratio + 1e-20);
  INFO("time stretch spurious/signal: " << decibels << " dB");

  REQUIRE(peakOf(output) > 0.1f);
  // Deliberately looser than the resampler: overlap-add always adds some
  // roughness. Anything worse than this is not "some roughness", it is broken.
  REQUIRE(decibels < -20.0);
}

TEST_CASE("time stretch does not repeat or drop audible chunks", "[integrity]") {
  // A cue or trim index error shows up as stuttering: the same fragment
  // emitted twice, or a piece missing. Neither changes the spectrum much, so
  // the energy tests above cannot see it - but it destroys intelligibility.
  TimeStretch stretch;
  stretch.configure(kSampleRate, 2);
  stretch.setRatio(1.02);

  // A slow ramp: every sample is unique and monotonic, so any repeat or jump
  // in the output is unambiguous.
  const size_t frames = kSampleRate * 2;
  std::vector<float> input(frames * 2);
  for (size_t f = 0; f < frames; ++f) {
    const float value = static_cast<float>(f) / static_cast<float>(frames);
    input[f * 2] = value;
    input[f * 2 + 1] = value;
  }

  std::vector<float> output;
  stretch.process(input.data(), frames, output);

  const size_t outFrames = output.size() / 2;
  REQUIRE(outFrames > frames / 2);

  // The ramp must stay monotonic within a small tolerance for the crossfades.
  size_t regressions = 0;
  for (size_t f = 1; f < outFrames; ++f) {
    if (output[f * 2] < output[(f - 1) * 2] - 0.01f) regressions += 1;
  }

  INFO("backwards steps in a monotonic ramp: " << regressions);
  REQUIRE(regressions == 0);
}

TEST_CASE("EQ mid-transition settings do not destroy the signal", "[integrity]") {
  // The bass swap runs the low band from 1 to 0 while another voice does the
  // reverse. Mid-sweep is where a badly conditioned filter blows up.
  ThreeBandEq eq;
  eq.configure(kSampleRate);

  const auto input = musicLike(kSampleRate * 2);

  for (const float low : {1.0f, 0.75f, 0.5f, 0.25f, 0.0f}) {
    auto buffer = input;
    eq.reset();
    eq.setBandGains(low, 1.0f, 1.0f);
    eq.snapToTargets();
    eq.process(buffer.data(), buffer.size() / 2);

    for (float sample : buffer) {
      REQUIRE(std::isfinite(sample));
      REQUIRE(std::abs(sample) < 4.0f);
    }

    const double ratio = spuriousEnergyRatio(buffer, kSampleRate / 2);
    const double decibels = 10.0 * std::log10(ratio + 1e-20);
    INFO("low gain " << low << ": spurious " << decibels << " dB");
    REQUIRE(decibels < -20.0);
  }
}

TEST_CASE("resampler into time stretch stays clean", "[integrity]") {
  // The real signal path: decode -> resample -> stretch -> ring. Each stage
  // passing alone does not guarantee the chain does.
  Resampler resampler;
  resampler.configure(44100, 2, kSampleRate);

  TimeStretch stretch;
  stretch.configure(kSampleRate, 2);
  stretch.setRatio(1.04);

  const auto input = musicLike(44100 * 4, 44100);

  std::vector<float> resampled;
  resampler.process(input.data(), input.size(), resampled);
  resampler.finish(resampled);

  std::vector<float> output;
  stretch.process(resampled.data(), resampled.size() / 2, output);
  stretch.finish(output);

  const double ratio = spuriousEnergyRatio(output, kSampleRate);
  const double decibels = 10.0 * std::log10(ratio + 1e-20);
  INFO("full chain spurious/signal: " << decibels << " dB");

  REQUIRE(peakOf(output) > 0.1f);
  REQUIRE(decibels < -20.0);
}
