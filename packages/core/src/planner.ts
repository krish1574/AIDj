import type { MixStyle } from './preferences';
import type { MusicalKey } from './track';

/**
 * Queue planner.
 *
 * Answers one question: which track should play next? It does NOT decide how
 * to mix into it - that is the transition planner's job (Milestone 5). Keeping
 * them apart matters because they fail differently: a bad queue is a taste
 * problem, a bad transition is an audible fault.
 *
 * Everything here is deterministic and explainable. Every decision carries the
 * component scores that produced it, which is what the debug view renders and
 * what a future ML ranker would be trained against.
 */

/** What the planner needs about a track. A subset of the full analysis. */
export interface PlannerTrack {
  id: number;
  title: string;
  /** Position in the user's original playlist. Load-bearing - see orderBonus. */
  originalIndex: number;
  durationMs: number;

  bpm: number;
  /** 0..1. Low confidence pushes the planner toward conservative choices. */
  bpmConfidence: number;
  /** Usually half or double `bpm`; lets the planner reconcile metrical levels. */
  alternateBpm: number;

  key: MusicalKey;
  keyConfidence: number;

  /** 0..1 whole-track energy. */
  energy: number;
  /** ITU-R BS.1770 integrated loudness, LUFS (negative). */
  integratedLufs: number;

  /** Mean vocal-activity probability, 0..1. Weak signal - weighted low. */
  vocalActivity: number;
}

export interface PlannerWeights {
  bpm: number;
  key: number;
  energy: number;
  loudness: number;
  order: number;
  vocalClash: number;
  /** Penalty applied per percent of tempo change beyond the comfort zone. */
  tempoStretchPenalty: number;
  /** Penalty for an energy step larger than `maxEnergyStep`. */
  energyJumpPenalty: number;
}

export interface PlannerOptions {
  weights: PlannerWeights;
  /**
   * How far ahead the planner may reach to pull a track forward. Bounded on
   * purpose: an unbounded search would happily rebuild the playlist into a
   * tempo ramp and destroy whatever intent the user had.
   */
  lookahead: number;
  /**
   * A candidate must beat the next-in-order track by this margin to displace
   * it. Without hysteresis, a rounding-level difference reorders the playlist,
   * which reads as the app shuffling music for no reason.
   */
  hysteresis: number;
  /** Tempo change we treat as free, as a fraction (0.04 = 4%). */
  tempoComfortZone: number;
  /** Beyond this fraction, a pairing is treated as not beat-matchable. */
  tempoMaxStretch: number;
  /** Energy step above which the jump penalty starts, 0..1. */
  maxEnergyStep: number;
  /** When false the planner returns the user's order untouched. */
  allowReordering: boolean;
}

/**
 * The three mix styles are three weight presets, not three code paths.
 *
 * - smooth: protects the listener from surprises. Tempo and loudness continuity
 *   dominate; the planner rarely reorders.
 * - balanced: the default. Playlist order still leads, but a clearly better
 *   pairing can win.
 * - energetic: tolerates bigger tempo and energy steps to build a set, and
 *   reorders more willingly.
 */
export const MIX_STYLES: Record<MixStyle, PlannerOptions> = {
  smooth: {
    weights: {
      bpm: 3.0,
      key: 1.5,
      energy: 1.5,
      loudness: 1.0,
      order: 3.0,
      vocalClash: 0.5,
      tempoStretchPenalty: 4.0,
      energyJumpPenalty: 3.0,
    },
    lookahead: 3,
    hysteresis: 0.35,
    tempoComfortZone: 0.03,
    tempoMaxStretch: 0.06,
    maxEnergyStep: 0.15,
    allowReordering: true,
  },
  balanced: {
    weights: {
      bpm: 2.5,
      key: 1.5,
      energy: 1.5,
      loudness: 0.8,
      order: 2.0,
      vocalClash: 0.5,
      tempoStretchPenalty: 3.0,
      energyJumpPenalty: 2.0,
    },
    lookahead: 4,
    hysteresis: 0.25,
    tempoComfortZone: 0.04,
    tempoMaxStretch: 0.08,
    maxEnergyStep: 0.25,
    allowReordering: true,
  },
  energetic: {
    weights: {
      bpm: 2.0,
      key: 1.0,
      energy: 2.5,
      loudness: 0.6,
      order: 1.2,
      vocalClash: 0.4,
      tempoStretchPenalty: 2.0,
      energyJumpPenalty: 0.8,
    },
    lookahead: 5,
    hysteresis: 0.18,
    tempoComfortZone: 0.05,
    tempoMaxStretch: 0.1,
    maxEnergyStep: 0.35,
    allowReordering: true,
  },
};

export interface ScoreBreakdown {
  bpm: number;
  key: number;
  energy: number;
  loudness: number;
  order: number;
  vocalClash: number;
  tempoPenalty: number;
  energyPenalty: number;
  total: number;
  /** Tempo ratio the transition engine would need. 1.0 = no stretch. */
  tempoRatio: number;
  /** True when matching required treating one tempo as half or double. */
  usedAlternateTempo: boolean;
}

export interface QueueItem {
  track: PlannerTrack;
  /** Null for the first track, which has no predecessor to be scored against. */
  score: ScoreBreakdown | null;
}

/** Pitch classes are a circle: the distance from B to C is one, not eleven. */
function pitchClassDistance(a: number, b: number): number {
  const raw = Math.abs(((a - b) % 12 + 12) % 12);
  return Math.min(raw, 12 - raw);
}

/**
 * Camelot-style key compatibility, 0..1.
 *
 * Mirrors keyCompatibility() in the C++ engine. Duplicated deliberately rather
 * than shared: the engine version runs during analysis, this one during
 * planning, and a JS/C++ bridge call per candidate pair would cost more than
 * the arithmetic.
 */
export function keyCompatibility(from: MusicalKey, to: MusicalKey): number {
  if (from.tonic === to.tonic && from.mode === to.mode) return 1.0;

  // Relative major/minor share every pitch class.
  const relative =
    from.mode !== to.mode &&
    ((from.mode === 'minor' && (from.tonic + 3) % 12 === to.tonic) ||
      (to.mode === 'minor' && (to.tonic + 3) % 12 === from.tonic));
  if (relative) return 0.9;

  // Distance around the circle of fifths, not the chromatic circle.
  const fifths = pitchClassDistance((from.tonic * 7) % 12, (to.tonic * 7) % 12);

  if (from.mode === to.mode) {
    if (fifths === 1) return 0.85;
    if (fifths === 2) return 0.55;
    if (fifths === 3) return 0.35;
    return 0.2;
  }

  if (fifths === 0) return 0.5;
  if (fifths === 1) return 0.45;
  if (fifths === 2) return 0.3;
  return 0.15;
}

/**
 * The tempo ratio needed to beat-match `to` against `from`, allowing for
 * half/double-time reconciliation.
 *
 * A 140 BPM track after a 70 BPM one is not a 100% tempo change - it is the
 * same pulse at a different metrical level, and a DJ would mix them directly.
 * Treating that as incompatible would reject perfectly good pairings, so the
 * planner tries the alternate readings and takes the closest.
 */
export function tempoMatch(
  from: PlannerTrack,
  to: PlannerTrack,
): { ratio: number; usedAlternate: boolean } {
  if (from.bpm <= 0 || to.bpm <= 0) {
    return { ratio: 1, usedAlternate: false };
  }

  const candidates: { bpm: number; alternate: boolean }[] = [
    { bpm: to.bpm, alternate: false },
    { bpm: to.bpm * 2, alternate: true },
    { bpm: to.bpm / 2, alternate: true },
  ];

  if (to.alternateBpm > 0) {
    candidates.push({ bpm: to.alternateBpm, alternate: true });
  }

  let best = { ratio: to.bpm / from.bpm, usedAlternate: false };
  let bestDeviation = Math.abs(Math.log2(best.ratio));

  for (const candidate of candidates) {
    const ratio = candidate.bpm / from.bpm;
    const deviation = Math.abs(Math.log2(ratio));
    if (deviation < bestDeviation) {
      bestDeviation = deviation;
      best = { ratio, usedAlternate: candidate.alternate };
    }
  }

  return best;
}

/**
 * Scores a transition from one track to another.
 *
 * Every term is bounded and named, so a queue decision can always be explained
 * as a sentence: "picked track 7 because tempo matched within 1% and the key
 * was adjacent, despite being two positions out of order".
 */
export function scoreTransition(
  from: PlannerTrack,
  to: PlannerTrack,
  currentIndex: number,
  options: PlannerOptions,
): ScoreBreakdown {
  const { weights } = options;

  const { ratio, usedAlternate } = tempoMatch(from, to);
  const stretch = Math.abs(ratio - 1);

  // Tempo compatibility decays from 1 at no stretch to 0 at the maximum.
  // Scaled by confidence: an unreliable BPM should not earn a high score just
  // because two guesses happen to agree.
  const tempoCloseness = Math.max(0, 1 - stretch / options.tempoMaxStretch);
  const confidence = Math.min(from.bpmConfidence, to.bpmConfidence);
  const bpmScore = weights.bpm * tempoCloseness * (0.4 + 0.6 * confidence);

  // Key is weighted by confidence too, and confidence is routinely low on real
  // material - which is the intended behaviour, not a bug. Key should nudge,
  // never decide.
  const keyConfidence = Math.min(from.keyConfidence, to.keyConfidence);
  const keyScore =
    weights.key * keyCompatibility(from.key, to.key) * keyConfidence;

  // Energy: reward small forward steps, tolerate flat, mildly penalise drops.
  // A set that only ever climbs is as wrong as one that never moves, so this
  // is deliberately gentle.
  const energyDelta = to.energy - from.energy;
  const energyFit =
    energyDelta >= 0
      ? Math.max(0, 1 - energyDelta / Math.max(options.maxEnergyStep, 1e-6))
      : Math.max(0, 1 - Math.abs(energyDelta) / (options.maxEnergyStep * 1.5));
  const energyScore = weights.energy * energyFit;

  // Loudness: a big LUFS gap means the transition needs a big gain correction,
  // which is where pumping and clipping come from.
  const loudnessGap = Math.abs(to.integratedLufs - from.integratedLufs);
  const loudnessScore = weights.loudness * Math.max(0, 1 - loudnessGap / 6);

  // Order preference decays with displacement from the user's sequence.
  const displacement = Math.max(0, to.originalIndex - currentIndex);
  const orderScore =
    weights.order * Math.max(0, 1 - displacement / (options.lookahead + 1));

  // Vocal clash: two vocal-heavy tracks overlapping is the most audible way a
  // transition sounds wrong. Weighted low because the detector is a heuristic.
  const clash = from.vocalActivity * to.vocalActivity;
  const vocalScore = -weights.vocalClash * clash;

  const excessStretch = Math.max(0, stretch - options.tempoComfortZone);
  const tempoPenalty = -weights.tempoStretchPenalty * (excessStretch * 25);

  const excessEnergy = Math.max(0, Math.abs(energyDelta) - options.maxEnergyStep);
  const energyPenalty = -weights.energyJumpPenalty * excessEnergy;

  const total =
    bpmScore +
    keyScore +
    energyScore +
    loudnessScore +
    orderScore +
    vocalScore +
    tempoPenalty +
    energyPenalty;

  return {
    bpm: bpmScore,
    key: keyScore,
    energy: energyScore,
    loudness: loudnessScore,
    order: orderScore,
    vocalClash: vocalScore,
    tempoPenalty,
    energyPenalty,
    total,
    tempoRatio: ratio,
    usedAlternateTempo: usedAlternate,
  };
}

/**
 * Builds the play order.
 *
 * Greedy with a bounded lookahead window and hysteresis. Not an optimal
 * ordering - finding one is a travelling-salesman problem, and "optimal" by a
 * scoring function nobody has validated against human listening would be false
 * precision anyway. Greedy is predictable, explainable and fast enough to
 * re-run whenever new analysis arrives.
 *
 * Tracks are never dropped: anything skipped stays available and its order
 * bonus rises as the playlist advances past it, so nothing gets stranded.
 */
export function planQueue(
  tracks: readonly PlannerTrack[],
  options: PlannerOptions = MIX_STYLES.balanced,
): QueueItem[] {
  if (tracks.length === 0) return [];

  const ordered = [...tracks].sort(
    (a, b) => a.originalIndex - b.originalIndex,
  );

  if (!options.allowReordering) {
    return ordered.map((track, index) => ({
      track,
      score:
        index === 0
          ? null
          : scoreTransition(
              ordered[index - 1] as PlannerTrack,
              track,
              index - 1,
              options,
            ),
    }));
  }

  const remaining = [...ordered];
  const queue: QueueItem[] = [];

  // The first track is the user's first track. Choosing a different opener
  // would be the most visible possible override of their intent, for the least
  // benefit - there is no preceding track to mix from.
  const first = remaining.shift() as PlannerTrack;
  queue.push({ track: first, score: null });

  let current = first;
  let position = 0;

  while (remaining.length > 0) {
    const window = remaining.slice(0, options.lookahead);

    let bestIndex = 0;
    let bestScore = scoreTransition(current, window[0] as PlannerTrack, position, options);

    for (let i = 1; i < window.length; i += 1) {
      const candidate = window[i] as PlannerTrack;
      const score = scoreTransition(current, candidate, position, options);

      // Hysteresis: a later candidate must be clearly better, not merely
      // better, before it displaces the next track in the user's order.
      if (score.total > bestScore.total + options.hysteresis) {
        bestScore = score;
        bestIndex = i;
      }
    }

    const chosen = remaining.splice(bestIndex, 1)[0] as PlannerTrack;
    queue.push({ track: chosen, score: bestScore });
    current = chosen;
    position += 1;
  }

  return queue;
}

/** Human-readable reason a track was chosen. Used by the debug view. */
export function explainChoice(item: QueueItem): string {
  if (item.score === null) return 'First track - your playlist order.';

  const parts: string[] = [];
  const stretchPercent = Math.abs(item.score.tempoRatio - 1) * 100;

  parts.push(
    stretchPercent < 0.5
      ? 'tempo already matches'
      : `tempo needs ${stretchPercent.toFixed(1)}%`,
  );
  if (item.score.usedAlternateTempo) parts.push('at half/double time');
  if (item.score.key > 0.5) parts.push('compatible key');
  if (item.score.tempoPenalty < -0.5) parts.push('large tempo change');
  if (item.score.energyPenalty < -0.5) parts.push('big energy jump');
  if (item.score.vocalClash < -0.3) parts.push('both vocal-heavy');

  return parts.join(', ');
}
