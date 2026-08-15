#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "analysis/AnalysisTypes.h"
#include "analysis/Fft.h"

namespace aidj {

/**
 * Spectral-flux onset envelope, the input to every rhythm stage.
 *
 * Flux is computed on a log-magnitude spectrum with half-wave rectification:
 * only *increases* in energy count, because a note starting is an increase and
 * a note ending is not an onset. Log compression matters more than it looks -
 * without it, loud sections dominate the envelope and quiet passages produce
 * no detectable beats at all.
 */
class OnsetEnvelope {
 public:
  explicit OnsetEnvelope(AnalysisFormat format);

  /** Feeds one mono block. Call repeatedly while streaming a file. */
  void process(const float* samples, size_t count);

  /** Flushes any tail and returns the finished envelope. */
  std::vector<float> finish();

  /**
   * Low-band flux (below ~200 Hz), tracking kick drums specifically.
   * Downbeat detection uses this rather than the full-band envelope because
   * bar starts are far more reliably marked by bass than by overall energy.
   */
  const std::vector<float>& lowBandFlux() const { return lowFlux_; }

  /** Per-frame RMS in dBFS, reused by the energy curve and structure stages. */
  const std::vector<float>& frameRmsDb() const { return rmsDb_; }

  /** Per-frame 12-bin chroma, reused by key detection. */
  const std::vector<float>& chromaFrames() const { return chroma_; }

  /** Per-frame vocal-band heuristic features. See AnalysisTypes.h. */
  const std::vector<float>& vocalBandRatio() const { return vocalRatio_; }

  size_t frameCount() const { return flux_.size(); }

 private:
  void processFrame();

  AnalysisFormat format_;
  Fft fft_;
  std::vector<float> window_;

  std::vector<float> buffer_;      // ring of pending input samples
  std::vector<float> frame_;       // windowed frame handed to the FFT
  std::vector<float> magnitudes_;  // current magnitude spectrum
  std::vector<float> previousLogMagnitudes_;

  std::vector<float> flux_;
  std::vector<float> lowFlux_;
  std::vector<float> rmsDb_;
  std::vector<float> chroma_;
  std::vector<float> vocalRatio_;

  /** Bin -> pitch class, or -1 outside the musical range. See constructor. */
  std::vector<int8_t> binPitchClass_;

  size_t lowBandMaxBin_ = 0;
  size_t vocalMinBin_ = 0;
  size_t vocalMaxBin_ = 0;
  bool hasPrevious_ = false;
};

/**
 * Normalises an envelope to zero median and unit scale, then subtracts a
 * moving average.
 *
 * Both steps are necessary before tempo estimation: absolute flux magnitude is
 * meaningless across tracks, and the moving-average subtraction is what stops
 * a slow build in loudness from being read as rhythmic periodicity.
 */
std::vector<float> normaliseEnvelope(const std::vector<float>& envelope,
                                     size_t movingAverageFrames);

}  // namespace aidj
