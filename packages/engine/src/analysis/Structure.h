#pragma once

#include <vector>

#include "analysis/AnalysisTypes.h"

namespace aidj {

/**
 * Section boundaries from a self-similarity matrix and Foote novelty.
 *
 * WHAT THIS DOES AND DOES NOT CLAIM
 *
 * It finds *boundaries*: points where the music changes character. That works
 * well and is what transition planning needs - a boundary is a musically
 * sensible place to mix.
 *
 * It does not label sections. Deciding "this is a chorus" needs repetition
 * analysis that is unreliable enough on real material that a wrong label is
 * worse than none. Sections carry energy and boundary strength instead, and
 * the planner reasons about those.
 *
 * For long continuous mixes - a 50 minute garba set is one file containing
 * dozens of songs - these boundaries are the song changes inside the mix, and
 * are the only way to enter such a file at a sensible point.
 */

struct StructureOptions {
  /**
   * Downsampling of analysis frames before building the matrix. The matrix is
   * O(n^2): an hour of audio at 93 fps is 335k frames, and a 335k x 335k
   * matrix is not merely slow, it does not fit in memory. At the default of
   * 32 that becomes ~10k frames, and the matrix stays a few hundred MB of work
   * done in strips rather than materialised.
   */
  int32_t downsample = 32;
  /** Half-width of the novelty kernel, in downsampled frames. */
  int32_t kernelHalfWidth = 32;
  /** Boundaries closer together than this are merged. */
  double minSectionMs = 8000.0;
};

struct StructureResult {
  std::vector<Section> sections;
  double introEndMs = 0.0;
  double outroStartMs = 0.0;
  std::vector<float> energyCurve;
  double energyCurveHopMs = 0.0;
  float overallEnergy = 0.0f;
};

/**
 * @param chromaFrames  12 values per analysis frame - timbre-invariant, so
 *                      boundaries track harmonic change rather than mixing
 *                      decisions like a filter sweep.
 * @param rmsDb         per-frame level, used for the energy curve and for
 *                      finding where the music actually starts and ends.
 */
StructureResult analyseStructure(const std::vector<float>& chromaFrames,
                                 const std::vector<float>& rmsDb,
                                 const AnalysisFormat& format,
                                 const StructureOptions& options = {});

}  // namespace aidj
