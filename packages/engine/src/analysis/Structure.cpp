#include "analysis/Structure.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace aidj {
namespace {

/** Level below the track's own loud sections that still counts as "playing". */
constexpr double kSilenceMarginDb = 35.0;

std::vector<std::vector<float>> downsampleChroma(
    const std::vector<float>& chromaFrames, int32_t factor) {
  const size_t frames = chromaFrames.size() / 12;
  const size_t blocks = frames / static_cast<size_t>(factor);

  std::vector<std::vector<float>> result;
  result.reserve(blocks);

  for (size_t block = 0; block < blocks; ++block) {
    std::vector<float> mean(12, 0.0f);
    for (int32_t i = 0; i < factor; ++i) {
      const size_t frame = block * static_cast<size_t>(factor) +
                           static_cast<size_t>(i);
      for (size_t pitch = 0; pitch < 12; ++pitch) {
        mean[pitch] += chromaFrames[frame * 12 + pitch];
      }
    }

    // L2-normalise so similarity is cosine of direction, independent of
    // loudness. Without this the matrix mostly reports where the track is
    // loud, which is not structure.
    float norm = 0.0f;
    for (float value : mean) norm += value * value;
    norm = std::sqrt(norm);
    if (norm > 1e-9f) {
      for (float& value : mean) value /= norm;
    }
    result.push_back(std::move(mean));
  }

  return result;
}

float similarity(const std::vector<float>& a, const std::vector<float>& b) {
  float dot = 0.0f;
  for (size_t i = 0; i < 12; ++i) dot += a[i] * b[i];
  return dot;
}

/**
 * Foote novelty: correlate a checkerboard kernel along the diagonal of the
 * self-similarity matrix. It peaks where the music before a point is
 * self-similar, the music after is self-similar, and the two differ - which
 * is exactly a section boundary.
 *
 * Only a band around the diagonal is ever computed, so cost is linear in
 * frames rather than quadratic.
 */
std::vector<float> footeNovelty(
    const std::vector<std::vector<float>>& features, int32_t halfWidth) {
  const size_t count = features.size();
  std::vector<float> novelty(count, 0.0f);
  if (count < static_cast<size_t>(halfWidth) * 2 + 1) return novelty;

  // Gaussian taper so frames near the kernel centre matter most.
  const size_t size = static_cast<size_t>(halfWidth) * 2;
  std::vector<float> taper(size);
  for (size_t i = 0; i < size; ++i) {
    const float x = (static_cast<float>(i) - static_cast<float>(halfWidth)) /
                    static_cast<float>(halfWidth);
    taper[i] = std::exp(-4.0f * x * x);
  }

  for (size_t centre = static_cast<size_t>(halfWidth);
       centre + static_cast<size_t>(halfWidth) < count; ++centre) {
    float sameBlock = 0.0f;
    float crossBlock = 0.0f;
    float sameWeight = 0.0f;
    float crossWeight = 0.0f;

    for (size_t i = 0; i < size; ++i) {
      for (size_t j = 0; j < size; ++j) {
        const size_t rowIndex = centre - static_cast<size_t>(halfWidth) + i;
        const size_t colIndex = centre - static_cast<size_t>(halfWidth) + j;
        const float weight = taper[i] * taper[j];
        const float value = similarity(features[rowIndex], features[colIndex]);

        const bool beforeRow = i < static_cast<size_t>(halfWidth);
        const bool beforeCol = j < static_cast<size_t>(halfWidth);

        if (beforeRow == beforeCol) {
          sameBlock += value * weight;
          sameWeight += weight;
        } else {
          crossBlock += value * weight;
          crossWeight += weight;
        }
      }
    }

    const float same = sameWeight > 0.0f ? sameBlock / sameWeight : 0.0f;
    const float cross = crossWeight > 0.0f ? crossBlock / crossWeight : 0.0f;
    novelty[centre] = std::max(0.0f, same - cross);
  }

  return novelty;
}

}  // namespace

StructureResult analyseStructure(const std::vector<float>& chromaFrames,
                                 const std::vector<float>& rmsDb,
                                 const AnalysisFormat& format,
                                 const StructureOptions& options) {
  StructureResult result;
  const size_t frames = rmsDb.size();
  if (frames == 0 || chromaFrames.size() < 12) return result;

  const double frameMs = 1000.0 / format.framesPerSecond();
  const double durationMs = format.frameToMs(static_cast<double>(frames));

  // Energy curve, normalised against the track's own dynamic range. Absolute
  // dBFS would make a quietly mastered track look low-energy throughout, which
  // is a mastering fact, not a musical one.
  double loudest = -200.0;
  double quietest = 200.0;
  for (double db : rmsDb) {
    loudest = std::max(loudest, db);
    quietest = std::min(quietest, db);
  }
  const double floorDb = std::max(quietest, loudest - kSilenceMarginDb);
  const double range = std::max(1.0, loudest - floorDb);

  result.energyCurve.reserve(frames);
  for (double db : rmsDb) {
    result.energyCurve.push_back(static_cast<float>(
        std::clamp((db - floorDb) / range, 0.0, 1.0)));
  }
  result.energyCurveHopMs = frameMs;
  result.overallEnergy = static_cast<float>(
      std::accumulate(result.energyCurve.begin(), result.energyCurve.end(), 0.0) /
      static_cast<double>(result.energyCurve.size()));

  // Intro and outro: where audible music actually starts and stops. Real files
  // have leading silence, fade-ins and trailing tails, and mixing into digital
  // silence sounds like a fault.
  const double audibleDb = floorDb + 6.0;
  size_t firstAudible = 0;
  while (firstAudible < frames && rmsDb[firstAudible] < audibleDb) {
    firstAudible += 1;
  }
  size_t lastAudible = frames;
  while (lastAudible > firstAudible && rmsDb[lastAudible - 1] < audibleDb) {
    lastAudible -= 1;
  }

  result.introEndMs = format.frameToMs(static_cast<double>(firstAudible));
  result.outroStartMs = format.frameToMs(static_cast<double>(lastAudible));

  const auto features = downsampleChroma(chromaFrames, options.downsample);
  if (features.size() < static_cast<size_t>(options.kernelHalfWidth) * 2 + 1) {
    // Too short to have structure worth reporting; one section is the honest
    // answer rather than inventing boundaries.
    Section whole;
    whole.startMs = 0.0;
    whole.endMs = durationMs;
    whole.energy = result.overallEnergy;
    whole.novelty = 0.0f;
    result.sections.push_back(whole);
    return result;
  }

  const std::vector<float> novelty =
      footeNovelty(features, options.kernelHalfWidth);

  // Adaptive threshold: mean plus one standard deviation. A fixed threshold
  // would over-segment busy music and under-segment sparse music.
  const double mean =
      std::accumulate(novelty.begin(), novelty.end(), 0.0) /
      static_cast<double>(novelty.size());
  double variance = 0.0;
  for (float value : novelty) {
    variance += (static_cast<double>(value) - mean) *
                (static_cast<double>(value) - mean);
  }
  variance /= static_cast<double>(novelty.size());
  const double threshold = mean + std::sqrt(variance);

  const double blockMs = frameMs * static_cast<double>(options.downsample);
  const size_t minGapBlocks = static_cast<size_t>(
      std::max(1.0, options.minSectionMs / blockMs));

  std::vector<std::pair<size_t, float>> peaks;
  for (size_t i = 1; i + 1 < novelty.size(); ++i) {
    if (novelty[i] <= threshold) continue;
    // Local maximum only, or a broad rise produces a cluster of boundaries.
    if (novelty[i] < novelty[i - 1] || novelty[i] < novelty[i + 1]) continue;
    if (!peaks.empty() && i - peaks.back().first < minGapBlocks) {
      // Too close to the previous boundary: keep whichever is stronger.
      if (novelty[i] > peaks.back().second) peaks.back() = {i, novelty[i]};
      continue;
    }
    peaks.push_back({i, novelty[i]});
  }

  float strongestNovelty = 1e-6f;
  for (const auto& peak : peaks) {
    strongestNovelty = std::max(strongestNovelty, peak.second);
  }

  const auto energyBetween = [&](double startMs, double endMs) {
    const size_t from = static_cast<size_t>(
        std::clamp(startMs / frameMs, 0.0, static_cast<double>(frames - 1)));
    const size_t to = static_cast<size_t>(
        std::clamp(endMs / frameMs, 0.0, static_cast<double>(frames)));
    if (to <= from) return 0.0f;
    double sum = 0.0;
    for (size_t i = from; i < to; ++i) sum += result.energyCurve[i];
    return static_cast<float>(sum / static_cast<double>(to - from));
  };

  double sectionStart = 0.0;
  float sectionNovelty = 0.0f;
  for (const auto& peak : peaks) {
    const double boundaryMs = format.frameToMs(
        static_cast<double>(peak.first * static_cast<size_t>(options.downsample)));

    Section section;
    section.startMs = sectionStart;
    section.endMs = boundaryMs;
    section.energy = energyBetween(sectionStart, boundaryMs);
    section.novelty = sectionNovelty;
    result.sections.push_back(section);

    sectionStart = boundaryMs;
    sectionNovelty = peak.second / strongestNovelty;
  }

  Section last;
  last.startMs = sectionStart;
  last.endMs = durationMs;
  last.energy = energyBetween(sectionStart, durationMs);
  last.novelty = sectionNovelty;
  result.sections.push_back(last);

  return result;
}

}  // namespace aidj
