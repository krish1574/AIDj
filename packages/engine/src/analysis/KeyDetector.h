#pragma once

#include <vector>

#include "analysis/AnalysisTypes.h"

namespace aidj {

struct KeyEstimate {
  MusicalKey key;
  float confidence = 0.0f;
};

/**
 * Krumhansl-Schmuckler key estimation over an averaged chroma profile.
 *
 * The chroma frames are averaged across the whole track, then correlated
 * against the twenty-four rotated major and minor profiles; the best
 * correlation wins. This is the standard published approach and is chosen for
 * being explainable rather than for being state of the art.
 *
 * Expect roughly 70% exact and ~85% within a Camelot-adjacent slot on real
 * material. Relative major/minor pairs share a pitch-class distribution almost
 * exactly, so they are the dominant confusion and the reason key must never
 * gate a transition on its own - only weight it.
 */
KeyEstimate estimateKey(const std::vector<float>& chromaFrames);

/**
 * Camelot-style compatibility, 0..1.
 *
 * 1.0 for the same key, high for a perfect fifth or the relative major/minor,
 * low for distant keys. Used as one weighted term in transition scoring, never
 * as a gate - real DJs mix across "incompatible" keys constantly when the
 * energy is right.
 */
float keyCompatibility(const MusicalKey& from, const MusicalKey& to);

}  // namespace aidj
