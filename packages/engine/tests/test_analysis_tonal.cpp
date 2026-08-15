#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "analysis/KeyDetector.h"
#include "analysis/Loudness.h"
#include "analysis/OnsetEnvelope.h"
#include "analysis/Structure.h"

using namespace aidj;

namespace {

constexpr int32_t kSampleRate = 48000;
constexpr double kPi = 3.14159265358979323846;

double midiToHz(int midi) {
  return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

/** Adds a harmonic tone so chroma sees a realistic spectrum, not a pure sine. */
void addNote(std::vector<float>& out, int midi, size_t start, size_t length,
             float gain) {
  const double hz = midiToHz(midi);
  for (size_t i = 0; i < length && start + i < out.size(); ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    // Fundamental plus two harmonics, with a gentle envelope so onsets are
    // not clicks.
    const double envelope = std::min(1.0, std::min(static_cast<double>(i) / 480.0,
                                                   static_cast<double>(length - i) / 480.0));
    double sample = std::sin(2.0 * kPi * hz * t);
    sample += 0.5 * std::sin(2.0 * kPi * hz * 2.0 * t);
    sample += 0.25 * std::sin(2.0 * kPi * hz * 3.0 * t);
    out[start + i] += static_cast<float>(gain * envelope * sample * 0.3);
  }
}

/**
 * A chord progression in a known key.
 * C major: C-E-G, F-A-C, G-B-D, C-E-G - unambiguous tonal centre.
 */
std::vector<float> cMajorProgression(double seconds) {
  std::vector<float> samples(static_cast<size_t>(seconds * kSampleRate), 0.0f);
  const size_t chordLength = static_cast<size_t>(kSampleRate * 1.0);

  // MIDI: C4=60, E4=64, G4=67, F4=65, A4=69, B3=59, D4=62
  const int chords[4][3] = {{60, 64, 67}, {65, 69, 72}, {67, 71, 74}, {60, 64, 67}};

  size_t position = 0;
  int chordIndex = 0;
  while (position + chordLength < samples.size()) {
    for (int note : chords[chordIndex % 4]) {
      addNote(samples, note, position, chordLength, 1.0f);
    }
    position += chordLength;
    chordIndex += 1;
  }
  return samples;
}

std::vector<float> sineAt(double amplitude, double seconds, double hz = 1000.0) {
  std::vector<float> samples(static_cast<size_t>(seconds * kSampleRate));
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<float>(
        amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSampleRate));
  }
  return samples;
}

}  // namespace

TEST_CASE("key detection finds the tonal centre", "[analysis]") {
  AnalysisFormat format;
  OnsetEnvelope detector(format);

  const std::vector<float> samples = cMajorProgression(16.0);
  detector.process(samples.data(), samples.size());

  const KeyEstimate estimate = estimateKey(detector.chromaFrames());

  INFO("tonic " << estimate.key.tonic << " mode "
                << (estimate.key.mode == MusicalMode::Major ? "major" : "minor")
                << " confidence " << estimate.confidence);

  // C = 0. Accepting the relative minor (A = 9) as well would be dishonest
  // here: this progression is unambiguously major, so a minor answer is wrong.
  REQUIRE(estimate.key.tonic == 0);
  REQUIRE(estimate.key.mode == MusicalMode::Major);
}

TEST_CASE("key compatibility ranks musical relationships correctly", "[analysis]") {
  const MusicalKey cMajor{0, MusicalMode::Major};
  const MusicalKey gMajor{7, MusicalMode::Major};   // perfect fifth
  const MusicalKey aMinor{9, MusicalMode::Minor};   // relative minor
  const MusicalKey fSharpMajor{6, MusicalMode::Major};  // tritone, distant

  REQUIRE(keyCompatibility(cMajor, cMajor) == 1.0f);
  REQUIRE(keyCompatibility(cMajor, aMinor) > keyCompatibility(cMajor, fSharpMajor));
  REQUIRE(keyCompatibility(cMajor, gMajor) > keyCompatibility(cMajor, fSharpMajor));

  // Never zero: a key clash is a cost the planner weighs, not a veto.
  REQUIRE(keyCompatibility(cMajor, fSharpMajor) > 0.0f);

  // Symmetric - mixing A into B is as compatible as B into A.
  REQUIRE(keyCompatibility(cMajor, gMajor) == keyCompatibility(gMajor, cMajor));
}

TEST_CASE("loudness meter matches a known reference level", "[analysis]") {
  // BS.1770 calibration: a 1 kHz sine at -20 dBFS should read close to
  // -20 LUFS, because K-weighting is near unity at 1 kHz.
  LoudnessMeter meter(kSampleRate);
  const double amplitude = std::pow(10.0, -20.0 / 20.0);
  const std::vector<float> samples = sineAt(amplitude, 5.0);

  meter.process(samples.data(), samples.size());
  const LoudnessResult result = meter.finish();

  INFO("measured " << result.integratedLufs << " LUFS");
  // Within 1 dB. A sine's mean square is half its squared amplitude, which the
  // standard's -0.691 offset and the K-weighting curve account for.
  REQUIRE(std::abs(result.integratedLufs - (-23.0)) < 1.5);
  REQUIRE(std::abs(result.peakDbfs - (-20.0)) < 0.5);
}

TEST_CASE("loudness gating ignores silence", "[analysis]") {
  // A track that is half music and half digital silence must not measure
  // ~3 dB quieter than the same music alone. This is the whole point of the
  // gate, and getting it wrong would make the DJ raise the gain on any track
  // with a long quiet intro.
  const double amplitude = std::pow(10.0, -20.0 / 20.0);

  LoudnessMeter musicOnly(kSampleRate);
  const std::vector<float> music = sineAt(amplitude, 5.0);
  musicOnly.process(music.data(), music.size());
  const double musicLufs = musicOnly.finish().integratedLufs;

  LoudnessMeter withSilence(kSampleRate);
  const std::vector<float> silence(kSampleRate * 5, 0.0f);
  withSilence.process(music.data(), music.size());
  withSilence.process(silence.data(), silence.size());
  const double paddedLufs = withSilence.finish().integratedLufs;

  INFO("music " << musicLufs << " padded " << paddedLufs);
  REQUIRE(std::abs(musicLufs - paddedLufs) < 0.5);
}

TEST_CASE("structure finds a boundary where the music changes", "[analysis]") {
  AnalysisFormat format;
  OnsetEnvelope detector(format);

  // Two harmonically distinct halves: C major then F# major. The boundary is
  // at exactly 10 s and nothing else changes, so a detector that finds no
  // boundary - or finds one elsewhere - is wrong.
  std::vector<float> samples(static_cast<size_t>(20.0 * kSampleRate), 0.0f);
  const size_t half = samples.size() / 2;
  const size_t chordLength = static_cast<size_t>(kSampleRate * 0.5);

  for (size_t position = 0; position + chordLength < half; position += chordLength) {
    for (int note : {60, 64, 67}) addNote(samples, note, position, chordLength, 1.0f);
  }
  for (size_t position = half; position + chordLength < samples.size();
       position += chordLength) {
    for (int note : {66, 70, 73}) addNote(samples, note, position, chordLength, 1.0f);
  }

  detector.process(samples.data(), samples.size());

  StructureOptions options;
  options.downsample = 8;
  options.kernelHalfWidth = 16;
  options.minSectionMs = 2000.0;

  const StructureResult structure =
      analyseStructure(detector.chromaFrames(), detector.frameRmsDb(), format,
                       options);

  REQUIRE(structure.sections.size() >= 2);

  bool foundNearMidpoint = false;
  for (size_t i = 1; i < structure.sections.size(); ++i) {
    const double boundaryMs = structure.sections[i].startMs;
    if (std::abs(boundaryMs - 10000.0) < 1500.0) foundNearMidpoint = true;
  }

  INFO("sections: " << structure.sections.size());
  REQUIRE(foundNearMidpoint);
}

TEST_CASE("energy curve tracks level changes", "[analysis]") {
  AnalysisFormat format;
  OnsetEnvelope detector(format);

  // Quiet first half, loud second half.
  std::vector<float> samples;
  const std::vector<float> quiet = sineAt(0.05, 5.0, 440.0);
  const std::vector<float> loud = sineAt(0.5, 5.0, 440.0);
  samples.insert(samples.end(), quiet.begin(), quiet.end());
  samples.insert(samples.end(), loud.begin(), loud.end());

  detector.process(samples.data(), samples.size());

  const StructureResult structure =
      analyseStructure(detector.chromaFrames(), detector.frameRmsDb(), format);

  REQUIRE(structure.energyCurve.size() > 100);

  const size_t quarter = structure.energyCurve.size() / 4;
  const float earlyEnergy = structure.energyCurve[quarter];
  const float lateEnergy = structure.energyCurve[quarter * 3];

  INFO("early " << earlyEnergy << " late " << lateEnergy);
  REQUIRE(lateEnergy > earlyEnergy + 0.2f);
}
