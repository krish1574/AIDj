#pragma once

#include <cstdint>
#include <vector>

namespace aidj {

/**
 * Analysis contracts, mirroring TrackAnalysis in packages/core/src/track.ts.
 *
 * Confidence values are 0..1 and are load-bearing: the transition planner is
 * required to degrade to safer, longer, non-beat-locked transitions when
 * confidence is low rather than trusting a grid it has no reason to trust.
 * Anything that cannot be determined is reported as low confidence, never as
 * a plausible-looking guess.
 */

/** Frame rate of every envelope in this file. */
struct AnalysisFormat {
  int32_t sampleRate = 48000;
  /** STFT hop in samples. 512 at 48 kHz gives ~93 frames per second. */
  int32_t hopSize = 512;
  int32_t frameSize = 2048;

  double framesPerSecond() const {
    return static_cast<double>(sampleRate) / static_cast<double>(hopSize);
  }

  /**
   * Milliseconds from the start of a frame to the instant it represents.
   *
   * A frame covers `frameSize` samples starting at `index * hopSize`, so its
   * content is centred half a window later. Timestamping frames at their start
   * makes every detected event systematically early by this amount - about
   * 21 ms at 2048/48000, which is a fifth of a beat at 128 BPM and enough to
   * make a beat-matched transition audibly wrong.
   */
  double frameCentreOffsetMs() const {
    return 500.0 * static_cast<double>(frameSize) /
           static_cast<double>(sampleRate);
  }

  /** Timestamp of analysis frame `index`. */
  double frameToMs(double index) const {
    return index * 1000.0 / framesPerSecond() + frameCentreOffsetMs();
  }
};

enum class MusicalMode : uint8_t { Major, Minor };

struct MusicalKey {
  /** 0 = C, 1 = C#, ... 11 = B. */
  int32_t tonic = 0;
  MusicalMode mode = MusicalMode::Major;
};

struct Section {
  double startMs = 0.0;
  double endMs = 0.0;
  /** Normalised 0..1 short-term loudness within the section. */
  float energy = 0.0f;
  /** Strength of the boundary that opened this section, 0..1. */
  float novelty = 0.0f;
};

struct TempoEstimate {
  double bpm = 0.0;
  float confidence = 0.0f;
  /**
   * The runner-up, almost always the half or double of `bpm`. Retained
   * because metrical-level ambiguity is the single most common tempo error,
   * and the queue planner can use it to reconcile two tracks that only
   * disagree by a factor of two.
   */
  double alternateBpm = 0.0;
};

struct BeatGrid {
  /** Beat times in milliseconds. Non-uniform: real tracks drift. */
  std::vector<double> beatsMs;
  /** Indices into beatsMs that begin a bar. */
  std::vector<int32_t> downbeatIndices;
  float beatConfidence = 0.0f;
  float downbeatConfidence = 0.0f;
  /** Beats per bar actually used. 4 unless evidence says otherwise. */
  int32_t beatsPerBar = 4;
};

struct LoudnessResult {
  /** ITU-R BS.1770-4 integrated loudness, LUFS. Negative for normal material. */
  double integratedLufs = 0.0;
  /** Sample peak in dBFS. True peak needs oversampling; see known-limitations. */
  double peakDbfs = 0.0;
};

struct TrackAnalysisResult {
  AnalysisFormat format;
  double durationMs = 0.0;

  TempoEstimate tempo;
  BeatGrid beats;

  MusicalKey key;
  float keyConfidence = 0.0f;

  LoudnessResult loudness;
  /** Whole-track energy, 0..1. */
  float energy = 0.0f;
  /** Short-term energy sampled per section-analysis frame, 0..1. */
  std::vector<float> energyCurve;
  double energyCurveHopMs = 0.0;

  std::vector<Section> sections;
  double introEndMs = 0.0;
  double outroStartMs = 0.0;

  /**
   * Heuristic vocal-presence probability per frame, 0..1. NOT stem
   * separation - see docs/known-limitations.md before trusting it.
   */
  std::vector<float> vocalActivity;
  double vocalActivityHopMs = 0.0;
};

}  // namespace aidj
