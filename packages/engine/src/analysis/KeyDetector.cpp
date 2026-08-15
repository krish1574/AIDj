#include "analysis/KeyDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace aidj {
namespace {

/**
 * Krumhansl-Kessler probe-tone profiles: how strongly each scale degree is
 * perceived as belonging to a key. Published values, used unmodified so the
 * result stays explainable and comparable with the literature.
 */
constexpr std::array<double, 12> kMajorProfile = {
    6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile = {
    6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

double correlation(const std::array<double, 12>& a,
                   const std::array<double, 12>& b) {
  const double meanA = std::accumulate(a.begin(), a.end(), 0.0) / 12.0;
  const double meanB = std::accumulate(b.begin(), b.end(), 0.0) / 12.0;

  double covariance = 0.0;
  double varianceA = 0.0;
  double varianceB = 0.0;
  for (size_t i = 0; i < 12; ++i) {
    const double da = a[i] - meanA;
    const double db = b[i] - meanB;
    covariance += da * db;
    varianceA += da * da;
    varianceB += db * db;
  }

  const double denominator = std::sqrt(varianceA * varianceB);
  return denominator > 1e-12 ? covariance / denominator : 0.0;
}

/** Distance around the circle of fifths, 0..6. */
int fifthsDistance(int fromTonic, int toTonic) {
  // Seven semitones is a fifth; multiplying by 7 modulo 12 maps the chromatic
  // circle onto the circle of fifths.
  const int from = (fromTonic * 7) % 12;
  const int to = (toTonic * 7) % 12;
  const int raw = ((to - from) % 12 + 12) % 12;
  return std::min(raw, 12 - raw);
}

}  // namespace

KeyEstimate estimateKey(const std::vector<float>& chromaFrames) {
  KeyEstimate estimate;
  if (chromaFrames.size() < 12) return estimate;

  const size_t frames = chromaFrames.size() / 12;
  if (frames == 0) return estimate;

  std::array<double, 12> average{};
  average.fill(0.0);

  for (size_t frame = 0; frame < frames; ++frame) {
    // Normalise each frame before averaging so loud passages do not dominate
    // the key estimate - a chorus should not outvote the rest of the track
    // simply for being louder.
    double frameSum = 0.0;
    for (size_t pitch = 0; pitch < 12; ++pitch) {
      frameSum += static_cast<double>(chromaFrames[frame * 12 + pitch]);
    }
    if (frameSum <= 1e-12) continue;
    for (size_t pitch = 0; pitch < 12; ++pitch) {
      average[pitch] +=
          static_cast<double>(chromaFrames[frame * 12 + pitch]) / frameSum;
    }
  }

  const double total = std::accumulate(average.begin(), average.end(), 0.0);
  if (total <= 1e-12) return estimate;

  double bestScore = -2.0;
  double secondScore = -2.0;
  MusicalKey bestKey;

  for (int tonic = 0; tonic < 12; ++tonic) {
    for (int modeIndex = 0; modeIndex < 2; ++modeIndex) {
      const auto& profile = modeIndex == 0 ? kMajorProfile : kMinorProfile;

      std::array<double, 12> rotated{};
      for (int pitch = 0; pitch < 12; ++pitch) {
        rotated[static_cast<size_t>(pitch)] =
            profile[static_cast<size_t>((pitch - tonic + 12) % 12)];
      }

      const double score = correlation(average, rotated);
      if (score > bestScore) {
        secondScore = bestScore;
        bestScore = score;
        bestKey.tonic = tonic;
        bestKey.mode = modeIndex == 0 ? MusicalMode::Major : MusicalMode::Minor;
      } else if (score > secondScore) {
        secondScore = score;
      }
    }
  }

  estimate.key = bestKey;
  // Margin over the runner-up, scaled so that a decisive win approaches 1.
  // Relative major/minor pairs correlate almost identically, so this is
  // routinely modest even when the answer is right - which is the honest
  // signal to the planner that key is weak evidence.
  estimate.confidence = static_cast<float>(
      std::clamp((bestScore - secondScore) * 2.0, 0.0, 1.0));

  return estimate;
}

float keyCompatibility(const MusicalKey& from, const MusicalKey& to) {
  if (from.tonic == to.tonic && from.mode == to.mode) return 1.0f;

  // Relative major/minor: A minor and C major share every pitch class, so they
  // are as compatible as two different keys get.
  const bool relativePair =
      from.mode != to.mode &&
      ((from.mode == MusicalMode::Minor &&
        (from.tonic + 3) % 12 == to.tonic) ||
       (to.mode == MusicalMode::Minor && (to.tonic + 3) % 12 == from.tonic));
  if (relativePair) return 0.9f;

  const int distance = fifthsDistance(from.tonic, to.tonic);

  if (from.mode == to.mode) {
    switch (distance) {
      case 1: return 0.85f;  // adjacent on the circle of fifths
      case 2: return 0.55f;
      case 3: return 0.35f;
      default: return 0.2f;
    }
  }

  // Different modes and not a relative pair: workable when close, poor when
  // not. Deliberately never zero - a clashing key is a cost, not a veto.
  switch (distance) {
    case 0: return 0.5f;  // parallel major/minor
    case 1: return 0.45f;
    case 2: return 0.3f;
    default: return 0.15f;
  }
}

}  // namespace aidj
