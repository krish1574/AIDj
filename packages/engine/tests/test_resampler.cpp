#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "AudioTypes.h"
#include "analysis/Fft.h"
#include "dsp/Resampler.h"

using aidj::kEngineSampleRate;
using aidj::Resampler;
using Catch::Approx;

namespace {

std::vector<float> makeSine(double frequencyHz, int32_t sampleRate,
                            int32_t channels, size_t frames) {
  std::vector<float> out(frames * static_cast<size_t>(channels));
  for (size_t f = 0; f < frames; ++f) {
    const auto value = static_cast<float>(
        std::sin(6.283185307179586 * frequencyHz * static_cast<double>(f) /
                 sampleRate));
    for (int32_t c = 0; c < channels; ++c) {
      out[f * static_cast<size_t>(channels) + static_cast<size_t>(c)] = value;
    }
  }
  return out;
}

float peak(const std::vector<float>& samples) {
  float result = 0.0f;
  for (const float sample : samples) {
    result = std::max(result, std::fabs(sample));
  }
  return result;
}

}  // namespace

TEST_CASE("Resampler passes 48 kHz stereo through untouched") {
  Resampler resampler;
  resampler.configure(48000, 2, kEngineSampleRate);
  REQUIRE(resampler.isPassThrough());

  const auto input = makeSine(1000.0, 48000, 2, 512);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);

  REQUIRE(output.size() == input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    REQUIRE(output[i] == input[i]);
  }
}

TEST_CASE("Resampler converts mono to stereo") {
  Resampler resampler;
  resampler.configure(48000, 1, kEngineSampleRate);
  REQUIRE_FALSE(resampler.isPassThrough());

  const auto input = makeSine(1000.0, 48000, 1, 4096);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);

  REQUIRE(output.size() % 2 == 0);
  REQUIRE(output.size() > 4000);
  // Both channels must carry the same signal, not silence on the right.
  for (size_t f = 0; f < output.size() / 2; ++f) {
    REQUIRE(output[f * 2] == Approx(output[f * 2 + 1]));
  }
}

TEST_CASE("Resampler produces the expected output length for 44.1 -> 48") {
  Resampler resampler;
  resampler.configure(44100, 2, kEngineSampleRate);

  const size_t inputFrames = 44100;  // one second
  const auto input = makeSine(440.0, 44100, 2, inputFrames);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);
  resampler.finish(output);

  const size_t outputFrames = output.size() / 2;
  // One second in, one second out, within a few frames of interpolation slack.
  REQUIRE(outputFrames > 47950);
  REQUIRE(outputFrames < 48050);
}

TEST_CASE("Resampler preserves amplitude and does not overshoot") {
  Resampler resampler;
  resampler.configure(44100, 2, kEngineSampleRate);

  const auto input = makeSine(440.0, 44100, 2, 44100);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);
  resampler.finish(output);

  // Catmull-Rom can overshoot slightly on steep slopes; anything beyond a few
  // percent would mean the interpolation is wrong, not merely imprecise.
  REQUIRE(peak(output) > 0.95f);
  REQUIRE(peak(output) < 1.05f);
}

TEST_CASE("Resampler keeps spurious energy far below the signal") {
  // The reason this resampler replaced a cubic interpolator. Cubic
  // interpolation leaves imaging artefacts only ~60-70 dB down, which is
  // audible in quiet passages and would contaminate any listening judgement of
  // transition quality.
  //
  // Feed one clean sine through 44.1 -> 48 kHz and measure everything that is
  // not the sine.
  Resampler resampler;
  resampler.configure(44100, 2, kEngineSampleRate);

  const auto input = makeSine(1000.0, 44100, 2, 44100);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);
  resampler.finish(output);

  // Take a windowed frame from the middle, clear of start and end transients.
  constexpr size_t kFftSize = 8192;
  const size_t startFrame = 24000;
  REQUIRE(output.size() / 2 > startFrame + kFftSize);

  aidj::Fft fft(kFftSize);
  const std::vector<float> window = aidj::hannWindow(kFftSize);

  std::vector<float> frame(kFftSize);
  for (size_t i = 0; i < kFftSize; ++i) {
    frame[i] = output[(startFrame + i) * 2] * window[i];
  }

  std::vector<float> magnitudes(kFftSize / 2 + 1, 0.0f);
  fft.magnitudeSpectrum(frame.data(), magnitudes.data());

  size_t peakBin = 0;
  for (size_t bin = 1; bin < magnitudes.size(); ++bin) {
    if (magnitudes[bin] > magnitudes[peakBin]) peakBin = bin;
  }

  // 1 kHz at 48 kHz over 8192 points lands near bin 170.
  INFO("peak bin " << peakBin);
  REQUIRE(peakBin > 160);
  REQUIRE(peakBin < 180);

  // Largest bin outside the signal's own skirt.
  //
  // The exclusion zone has to clear the *analysis window's* sidelobes, not
  // just its main lobe. A Hann window's nearby sidelobes sit around -50 dB, so
  // an earlier version of this test excluding only +-4 bins measured the
  // window rather than the resampler and reported a fixed -50 dB no matter
  // what the filter did. 32 bins puts the measurement into Hann's far
  // roll-off, well below the level being asserted.
  constexpr size_t kSkirtBins = 32;
  float spurious = 0.0f;
  for (size_t bin = 1; bin < magnitudes.size(); ++bin) {
    const size_t distance = bin > peakBin ? bin - peakBin : peakBin - bin;
    if (distance <= kSkirtBins) continue;
    spurious = std::max(spurious, magnitudes[bin]);
  }

  const float ratioDb =
      20.0f * std::log10(spurious / magnitudes[peakBin] + 1e-20f);
  INFO("worst spurious component: " << ratioDb << " dB");

  // Measured at -96.7 dB. The threshold is set just below that so a
  // regression in the filter or the phase interpolation fails here rather than
  // being discovered by ear during transition tuning.
  REQUIRE(ratioDb < -90.0f);
}

TEST_CASE("Resampler does not fold near-Nyquist content back into the audible band") {
  // The case that decides whether the cutoff needs margin below Nyquist.
  //
  // A 20 kHz tone at 44.1 kHz sits close to that format's 22.05 kHz ceiling.
  // Its first image lands at 24.1 kHz, which at 48 kHz output reflects to
  // 23.9 kHz - and if the reconstruction filter has not rolled off by then,
  // some of it survives. A 1 kHz test tone cannot detect this at all, because
  // it has no energy anywhere near the transition band.
  Resampler resampler;
  resampler.configure(44100, 2, kEngineSampleRate);

  const auto input = makeSine(20000.0, 44100, 2, 44100);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);
  resampler.finish(output);

  constexpr size_t kFftSize = 8192;
  const size_t startFrame = 24000;
  REQUIRE(output.size() / 2 > startFrame + kFftSize);

  aidj::Fft fft(kFftSize);
  const std::vector<float> window = aidj::hannWindow(kFftSize);
  std::vector<float> frame(kFftSize);
  for (size_t i = 0; i < kFftSize; ++i) {
    frame[i] = output[(startFrame + i) * 2] * window[i];
  }

  std::vector<float> magnitudes(kFftSize / 2 + 1, 0.0f);
  fft.magnitudeSpectrum(frame.data(), magnitudes.data());

  size_t peakBin = 0;
  for (size_t bin = 1; bin < magnitudes.size(); ++bin) {
    if (magnitudes[bin] > magnitudes[peakBin]) peakBin = bin;
  }

  // Everything well away from the tone itself. Anything substantial here was
  // manufactured by the resampler.
  constexpr size_t kSkirtBins = 32;
  float spurious = 0.0f;
  for (size_t bin = 1; bin < magnitudes.size(); ++bin) {
    const size_t distance = bin > peakBin ? bin - peakBin : peakBin - bin;
    if (distance <= kSkirtBins) continue;
    spurious = std::max(spurious, magnitudes[bin]);
  }

  const float ratioDb =
      20.0f * std::log10(spurious / magnitudes[peakBin] + 1e-20f);
  INFO("peak bin " << peakBin << ", worst spurious: " << ratioDb << " dB");
  // Measured at -91.3 dB. This is the test that actually constrains the
  // cutoff margin and the phase interpolation: with the cutoff at Nyquist it
  // reads -24.8 dB, and without phase interpolation -59.9 dB.
  REQUIRE(ratioDb < -80.0f);
}

TEST_CASE("Resampler rejects content above the new Nyquist when downsampling") {
  // The one resampling mistake that is unmistakably audible: decimating
  // without lowering the cutoff folds everything above the new Nyquist back
  // into the audible band. A 30 kHz tone downsampled 96 -> 48 kHz would alias
  // to 18 kHz - a loud whistle that was never in the source.
  Resampler resampler;
  resampler.configure(96000, 2, kEngineSampleRate);

  const auto input = makeSine(30000.0, 96000, 2, 96000);
  std::vector<float> output;
  resampler.process(input.data(), input.size(), output);
  resampler.finish(output);

  REQUIRE(output.size() > 8192 * 2);

  // Skip the filter's settling transient at the start.
  const size_t startFrame = 8000;
  constexpr size_t kFftSize = 8192;
  REQUIRE(output.size() / 2 > startFrame + kFftSize);

  aidj::Fft fft(kFftSize);
  const std::vector<float> window = aidj::hannWindow(kFftSize);
  std::vector<float> frame(kFftSize);
  for (size_t i = 0; i < kFftSize; ++i) {
    frame[i] = output[(startFrame + i) * 2] * window[i];
  }

  std::vector<float> magnitudes(kFftSize / 2 + 1, 0.0f);
  fft.magnitudeSpectrum(frame.data(), magnitudes.data());

  float loudest = 0.0f;
  for (float magnitude : magnitudes) loudest = std::max(loudest, magnitude);

  // The tone is above the output Nyquist, so the correct result is near
  // silence. Anything substantial here is an alias.
  const float levelDb = 20.0f * std::log10(loudest / static_cast<float>(kFftSize) + 1e-20f);
  INFO("residual level: " << levelDb << " dB");
  REQUIRE(levelDb < -60.0f);
}

TEST_CASE("Resampler has no seam when input arrives in small chunks") {
  const auto input = makeSine(440.0, 44100, 2, 44100);

  Resampler wholeAtOnce;
  wholeAtOnce.configure(44100, 2, kEngineSampleRate);
  std::vector<float> reference;
  wholeAtOnce.process(input.data(), input.size(), reference);

  Resampler chunked;
  chunked.configure(44100, 2, kEngineSampleRate);
  std::vector<float> streamed;
  constexpr size_t kChunk = 512;  // not a multiple of the resample ratio
  for (size_t offset = 0; offset < input.size(); offset += kChunk) {
    const size_t count = std::min(kChunk, input.size() - offset);
    chunked.process(input.data() + offset, count, streamed);
  }

  REQUIRE(streamed.size() == reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    // Bit-identical is the requirement: a chunk boundary must not perturb the
    // interpolation state at all, or every buffer boundary becomes a click.
    REQUIRE(streamed[i] == Approx(reference[i]).margin(1e-6f));
  }
}
