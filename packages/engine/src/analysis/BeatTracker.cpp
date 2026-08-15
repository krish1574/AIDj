#include "analysis/BeatTracker.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace aidj {
namespace {

/** Centre of the tempo prior, and its width in octaves. */
constexpr double kPriorCentreBpm = 120.0;
constexpr double kPriorWidthOctaves = 1.0;

/**
 * Weight of the timing penalty against onset strength in the DP.
 * Ellis uses ~6-7; higher values track a steadier pulse and are less willing
 * to follow syncopation, which is what we want for beat-matched mixing.
 */
constexpr double kTightness = 7.0;

double bpmToPeriodFrames(double bpm, double framesPerSecond) {
  return framesPerSecond * 60.0 / bpm;
}

double periodFramesToBpm(double periodFrames, double framesPerSecond) {
  return framesPerSecond * 60.0 / periodFrames;
}

/** Log-Gaussian prior: penalises tempi far from kPriorCentreBpm. */
double tempoPrior(double bpm) {
  const double octaves = std::log2(bpm / kPriorCentreBpm);
  return std::exp(-0.5 * (octaves / kPriorWidthOctaves) *
                  (octaves / kPriorWidthOctaves));
}

}  // namespace

TempoEstimate estimateTempo(const std::vector<float>& onsetEnvelope,
                            double framesPerSecond) {
  TempoEstimate estimate;
  if (onsetEnvelope.size() < 16 || framesPerSecond <= 0.0) return estimate;

  const size_t minLag = static_cast<size_t>(
      std::floor(bpmToPeriodFrames(kMaxBpm, framesPerSecond)));
  const size_t maxLag = static_cast<size_t>(
      std::ceil(bpmToPeriodFrames(kMinBpm, framesPerSecond)));

  if (maxLag >= onsetEnvelope.size() || minLag < 1 || minLag >= maxLag) {
    return estimate;
  }

  // Unbiased autocorrelation: normalising by the overlap length stops long
  // lags from being penalised simply for having fewer terms.
  std::vector<double> raw(maxLag + 1, 0.0);
  std::vector<double> scores(maxLag + 1, 0.0);
  for (size_t lag = minLag; lag <= maxLag; ++lag) {
    double sum = 0.0;
    const size_t count = onsetEnvelope.size() - lag;
    for (size_t i = 0; i < count; ++i) {
      sum += static_cast<double>(onsetEnvelope[i]) *
             static_cast<double>(onsetEnvelope[i + lag]);
    }
    const double bpm = periodFramesToBpm(static_cast<double>(lag),
                                         framesPerSecond);
    raw[lag] = sum / static_cast<double>(count);
    scores[lag] = raw[lag] * tempoPrior(bpm);
  }

  size_t bestLag = minLag;
  for (size_t lag = minLag; lag <= maxLag; ++lag) {
    if (scores[lag] > scores[bestLag]) bestLag = lag;
  }

  if (scores[bestLag] <= 0.0) return estimate;

  // Octave correction.
  //
  // For any periodic signal, autocorrelation peaks just as strongly at twice
  // the true period as at the period itself, so the prior alone decides - and
  // it reliably picks the slower tempo for anything fast (174 BPM was read as
  // 87). Halving the lag is only correct when the faster period genuinely
  // correlates, so the test is made on raw correlation with the prior removed;
  // a true 87 BPM track has nothing on the off-beats and fails it.
  //
  // 0.80 is deliberately demanding: a wrong doubling is worse than a wrong
  // halving, because it puts a beat where no onset exists.
  constexpr double kOctaveThreshold = 0.80;
  size_t chosenLag = bestLag;
  while (chosenLag / 2 >= minLag &&
         raw[chosenLag / 2] >= kOctaveThreshold * raw[chosenLag]) {
    chosenLag /= 2;
  }

  const double bestScore = scores[chosenLag];
  estimate.bpm = periodFramesToBpm(static_cast<double>(chosenLag),
                                   framesPerSecond);
  bestLag = chosenLag;

  // Two different uncertainties live here and must not be conflated.
  //
  // Metrical-level ambiguity (is it 87 or 174?) is expected and mostly
  // harmless: both grids land on real beats, and the planner can reconcile
  // them by doubling. It is reported through `alternateBpm`.
  //
  // Pulse ambiguity (is there any steady beat at all?) is what `confidence`
  // must express, so it is measured against the best competitor that is *not*
  // an octave relative of the winner. Counting the octave partner as a
  // competitor would report near-zero confidence for every clean 4/4 track,
  // which is precisely backwards.
  const size_t exclusion = std::max<size_t>(2, bestLag / 8);

  const auto isOctaveRelated = [&](size_t lag) {
    for (const double ratio : {0.25, 0.5, 2.0, 4.0}) {
      const double related = static_cast<double>(bestLag) * ratio;
      if (std::abs(static_cast<double>(lag) - related) <=
          std::max(2.0, related * 0.12)) {
        return true;
      }
    }
    return false;
  };

  size_t rivalLag = 0;
  double rivalScore = 0.0;
  size_t octaveLag = 0;
  double octaveScore = 0.0;

  for (size_t lag = minLag; lag <= maxLag; ++lag) {
    const size_t distance = lag > bestLag ? lag - bestLag : bestLag - lag;
    if (distance <= exclusion) continue;

    if (isOctaveRelated(lag)) {
      if (scores[lag] > octaveScore) {
        octaveScore = scores[lag];
        octaveLag = lag;
      }
    } else if (scores[lag] > rivalScore) {
      rivalScore = scores[lag];
      rivalLag = lag;
    }
  }

  // Prefer reporting the octave partner as the alternate, since that is the
  // ambiguity a caller can actually act on.
  const size_t alternateLag = octaveLag != 0 ? octaveLag : rivalLag;
  if (alternateLag != 0) {
    estimate.alternateBpm = periodFramesToBpm(static_cast<double>(alternateLag),
                                              framesPerSecond);
  }

  estimate.confidence = static_cast<float>(
      std::clamp((bestScore - rivalScore) / bestScore, 0.0, 1.0));

  return estimate;
}

BeatGrid trackBeats(const std::vector<float>& onsetEnvelope,
                    const std::vector<float>& lowBandEnvelope,
                    const TempoEstimate& tempo, const AnalysisFormat& format) {
  const double framesPerSecond = format.framesPerSecond();
  BeatGrid grid;
  if (onsetEnvelope.empty() || tempo.bpm <= 0.0 || framesPerSecond <= 0.0) {
    return grid;
  }

  const double period = bpmToPeriodFrames(tempo.bpm, framesPerSecond);
  if (period < 2.0 || period >= static_cast<double>(onsetEnvelope.size())) {
    return grid;
  }

  const size_t frames = onsetEnvelope.size();
  std::vector<double> cumulative(frames, 0.0);
  std::vector<int64_t> backlink(frames, -1);

  // Search window around one period back, wide enough to follow real tempo
  // drift but not so wide that it can skip a beat.
  const int searchFrom = static_cast<int>(std::round(-2.0 * period));
  const int searchTo = static_cast<int>(std::round(-0.5 * period));

  for (size_t i = 0; i < frames; ++i) {
    double bestScore = -1e30;
    int64_t bestPrevious = -1;

    for (int offset = searchFrom; offset <= searchTo; ++offset) {
      const int64_t previous = static_cast<int64_t>(i) + offset;
      if (previous < 0) continue;

      const double interval = static_cast<double>(i) -
                              static_cast<double>(previous);
      // Squared log deviation from the ideal period: symmetric in tempo ratio,
      // so being 10% fast costs the same as being 10% slow.
      const double deviation = std::log(interval / period);
      const double penalty = -kTightness * deviation * deviation;
      const double score = cumulative[static_cast<size_t>(previous)] + penalty;

      if (score > bestScore) {
        bestScore = score;
        bestPrevious = previous;
      }
    }

    if (bestPrevious < 0) {
      cumulative[i] = static_cast<double>(onsetEnvelope[i]);
      backlink[i] = -1;
    } else {
      cumulative[i] = static_cast<double>(onsetEnvelope[i]) + bestScore;
      backlink[i] = bestPrevious;
    }
  }

  // Start the backtrace from the best score in the final period, not the
  // global maximum: the DP score grows with time, so the global maximum is
  // almost always simply the last frame.
  const size_t tailStart = frames > static_cast<size_t>(period)
                               ? frames - static_cast<size_t>(period)
                               : 0;
  size_t endFrame = tailStart;
  for (size_t i = tailStart; i < frames; ++i) {
    if (cumulative[i] > cumulative[endFrame]) endFrame = i;
  }

  std::vector<size_t> beatFrames;
  for (int64_t at = static_cast<int64_t>(endFrame); at >= 0;
       at = backlink[static_cast<size_t>(at)]) {
    beatFrames.push_back(static_cast<size_t>(at));
    if (backlink[static_cast<size_t>(at)] < 0) break;
  }
  std::reverse(beatFrames.begin(), beatFrames.end());

  if (beatFrames.size() < 2) return grid;

  grid.beatsMs.reserve(beatFrames.size());
  for (size_t frame : beatFrames) {
    // frameToMs, not frame*msPerFrame: see AnalysisFormat::frameCentreOffsetMs.
    grid.beatsMs.push_back(format.frameToMs(static_cast<double>(frame)));
  }

  // Beat confidence: how much stronger the onsets are ON the beats than
  // exactly BETWEEN them.
  //
  // Comparing beats against the overall mean does not work - the DP places
  // beats on peaks by construction, so that ratio saturates at 1.0 for every
  // track including material with no pulse at all, which is worthless to a
  // planner that is supposed to distrust weak grids. Midpoints are the honest
  // control: a real pulse is much stronger on the beat than off it, while
  // noise or speech is equally (un)eventful in both places.
  double onBeatSum = 0.0;
  double offBeatSum = 0.0;
  size_t offBeatCount = 0;

  for (size_t i = 0; i < beatFrames.size(); ++i) {
    onBeatSum += onsetEnvelope[beatFrames[i]];
    if (i + 1 < beatFrames.size()) {
      const size_t midpoint = (beatFrames[i] + beatFrames[i + 1]) / 2;
      if (midpoint < onsetEnvelope.size()) {
        offBeatSum += onsetEnvelope[midpoint];
        offBeatCount += 1;
      }
    }
  }

  const double onBeatMean = onBeatSum / static_cast<double>(beatFrames.size());
  const double offBeatMean =
      offBeatCount > 0 ? offBeatSum / static_cast<double>(offBeatCount) : 0.0;

  (void)offBeatMean;
  (void)onBeatMean;
  (void)offBeatCount;

  // Beat confidence: normalised autocorrelation of the onset envelope at the
  // beat period.
  //
  // This asks the only question that actually matters - is the envelope
  // periodic at this rate? - rather than asking where the beats sit relative
  // to their neighbours. Three earlier attempts all failed on that distinction
  // and are recorded so they are not retried:
  //
  //  - on-beat mean vs overall mean: saturated at 1.0 for everything including
  //    speech, because the DP places beats on local maxima by construction;
  //  - contrast x uniformity of on-beat strengths: backwards on real music,
  //    scoring a mastered DJ edit 0.05 and a monotonous voice memo 0.40,
  //    because varying onset strength is what music does;
  //  - fraction of beats in the envelope's top quartile: after normalisation
  //    most frames are zero, so the threshold sits near zero and every
  //    DP-chosen peak clears it - noise scored 0.97.
  //
  // All three shared one flaw: they compared beats against other frames, and
  // any periodic grid the DP emits lands on peaks whatever the input. The
  // autocorrelation coefficient has no such loophole. It is bounded by
  // construction, near zero for unstructured audio, and high only when the
  // signal genuinely repeats at this period.
  double energy = 0.0;
  for (float value : onsetEnvelope) {
    energy += static_cast<double>(value) * static_cast<double>(value);
  }

  double correlation = 0.0;
  const size_t lag = static_cast<size_t>(std::llround(period));
  if (lag > 0 && lag < onsetEnvelope.size()) {
    for (size_t i = 0; i + lag < onsetEnvelope.size(); ++i) {
      correlation += static_cast<double>(onsetEnvelope[i]) *
                     static_cast<double>(onsetEnvelope[i + lag]);
    }
  }

  grid.beatConfidence =
      energy > 1e-9
          ? static_cast<float>(std::clamp(correlation / energy, 0.0, 1.0))
          : 0.0f;

  // Downbeats: try each phase within the bar and pick the one with the most
  // low-band (kick) energy. Bass marks bar starts far more reliably than
  // overall energy does.
  grid.beatsPerBar = 4;
  if (!lowBandEnvelope.empty() &&
      beatFrames.size() >= static_cast<size_t>(grid.beatsPerBar)) {
    double bestPhaseScore = -1.0;
    int bestPhase = 0;
    std::vector<double> phaseScores(static_cast<size_t>(grid.beatsPerBar), 0.0);

    for (int phase = 0; phase < grid.beatsPerBar; ++phase) {
      double sum = 0.0;
      size_t count = 0;
      for (size_t i = static_cast<size_t>(phase); i < beatFrames.size();
           i += static_cast<size_t>(grid.beatsPerBar)) {
        const size_t frame = beatFrames[i];
        if (frame < lowBandEnvelope.size()) {
          sum += static_cast<double>(lowBandEnvelope[frame]);
          count += 1;
        }
      }
      const double mean = count > 0 ? sum / static_cast<double>(count) : 0.0;
      phaseScores[static_cast<size_t>(phase)] = mean;
      if (mean > bestPhaseScore) {
        bestPhaseScore = mean;
        bestPhase = phase;
      }
    }

    for (size_t i = static_cast<size_t>(bestPhase); i < beatFrames.size();
         i += static_cast<size_t>(grid.beatsPerBar)) {
      grid.downbeatIndices.push_back(static_cast<int32_t>(i));
    }

    // Confidence is the winning phase's margin over the mean of the others.
    // Four phases that score alike mean we genuinely cannot hear the bar.
    double otherSum = 0.0;
    for (int phase = 0; phase < grid.beatsPerBar; ++phase) {
      if (phase != bestPhase) otherSum += phaseScores[static_cast<size_t>(phase)];
    }
    const double otherMean = otherSum / static_cast<double>(grid.beatsPerBar - 1);
    grid.downbeatConfidence =
        bestPhaseScore > 1e-9
            ? static_cast<float>(
                  std::clamp((bestPhaseScore - otherMean) / bestPhaseScore, 0.0,
                             1.0))
            : 0.0f;
  }

  return grid;
}

}  // namespace aidj
