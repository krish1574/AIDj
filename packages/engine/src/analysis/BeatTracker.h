#pragma once

#include <vector>

#include "analysis/AnalysisTypes.h"

namespace aidj {

/** Tempo search range. Outside this, half/double folding handles the rest. */
constexpr double kMinBpm = 70.0;
constexpr double kMaxBpm = 190.0;

/**
 * Estimates tempo from an onset envelope by autocorrelation weighted toward
 * musically plausible tempi.
 *
 * The weighting is not decoration. Raw autocorrelation of a 4/4 track peaks
 * just as strongly at half and double the true tempo, and at the bar period.
 * A log-Gaussian prior centred at 120 BPM is the standard, explainable way to
 * break that tie, and the runner-up is reported so the caller can see when the
 * decision was close.
 */
TempoEstimate estimateTempo(const std::vector<float>& onsetEnvelope,
                            double framesPerSecond);

/**
 * Beat tracking by dynamic programming (Ellis, 2007).
 *
 * Maximises a trade-off between landing on onset peaks and keeping the
 * inter-beat interval near the estimated period. Chosen over simple peak
 * picking because it produces a globally consistent grid: a missed onset in a
 * quiet bar does not desynchronise everything after it, which is exactly the
 * failure that makes a DJ transition land wrong.
 */
BeatGrid trackBeats(const std::vector<float>& onsetEnvelope,
                    const std::vector<float>& lowBandEnvelope,
                    const TempoEstimate& tempo, const AnalysisFormat& format);

}  // namespace aidj
