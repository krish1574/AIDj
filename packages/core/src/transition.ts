import type { TransitionLength } from './preferences';

/**
 * Transition planner.
 *
 * Decides HOW to move from one track into the next: where to start, where to
 * cue the incoming track, how long to take, what tempo to settle on, and what
 * the gain and EQ should be doing throughout.
 *
 * Separate from the queue planner on purpose. The queue planner answers "what
 * next" from metadata and can be wrong in a way that is merely a matter of
 * taste. This is answering a question where being wrong is audible: a
 * transition that lands off the beat sounds broken to anybody, whether or not
 * they know why.
 *
 * Nothing here touches audio. It produces a plan that the engine compiles into
 * a sample-accurate automation timeline; keeping the decisions in typed,
 * testable code means the musical logic can be argued about without a device.
 */

/** Everything the planner needs about one side of a transition. */
export interface TransitionTrack {
  id: number;
  title: string;
  durationMs: number;

  bpm: number;
  /** 0..1. Below `MIN_BEAT_CONFIDENCE` the grid is not trusted for matching. */
  beatConfidence: number;
  /** Beat times in milliseconds. Empty when analysis failed. */
  beatsMs: readonly number[];
  /** Indices into beatsMs that begin a bar. */
  downbeatIndices: readonly number[];
  beatsPerBar: number;

  /**
   * Milliseconds; audible music starts here, after any leading silence.
   *
   * NOT the end of a musical intro. This is a silence boundary, which is a
   * very different thing: a track that opens on a vocal hook has introEndMs
   * at its first syllable. Use `sections` to find somewhere sensible to mix
   * into; this only says where sound begins.
   */
  introEndMs: number;
  /** Milliseconds; audible music ends here. Also a silence boundary. */
  outroStartMs: number;

  /**
   * Structural sections with their relative energy, from the analyser.
   *
   * This is what makes a transition musical rather than merely aligned. A DJ
   * mixes out of a calm passage and into one, because dropping a vocal hook
   * underneath another vocal is the most obvious way a mix sounds wrong - and
   * cueing to "the first audible moment" does exactly that on any track that
   * opens on its hook, which most modern edits do.
   */
  sections: readonly { startMs: number; endMs: number; energy: number }[];

  /** ITU-R BS.1770 integrated loudness, LUFS (negative). */
  integratedLufs: number;
}

/**
 * Below this, the beat grid is not reliable enough to align against.
 *
 * Set from measurement rather than taste: on a real library, speech and
 * non-musical recordings scored 0.20-0.38 while mastered music scored
 * 0.50-0.83. 0.45 sits in the gap. A grid below it is not merely imprecise -
 * it is arbitrary, and beat-matching to an arbitrary grid sounds worse than
 * not beat-matching at all.
 */
export const MIN_BEAT_CONFIDENCE = 0.45;

/**
 * Largest tempo change we will ask a time-stretcher for, as a fraction.
 *
 * Beyond roughly this, pitch-preserving stretch starts to smear transients
 * audibly, and a DJ would pick a different record rather than force it.
 */
export const MAX_TEMPO_STRETCH = 0.08;

/** Loudness every track is nudged towards, so transitions do not jump level. */
export const TARGET_LUFS = -14;

/**
 * Gain correction is clamped to this. A quiet track pushed up by 12 dB does
 * not become a loud track, it becomes a noisy one - and its dynamics are the
 * artistic choice of whoever mastered it.
 */
export const MAX_GAIN_CORRECTION_DB = 6;

export type TransitionStyle =
  /** Beat- and phrase-aligned, tempo matched. What the product is for. */
  | 'phraseAligned'
  /** Beat aligned but not phrase aligned; the grids agree, the bars do not. */
  | 'beatAligned'
  /** No usable grid: a plain equal-power crossfade at a sensible point. */
  | 'crossfade'
  /** Last resort: no overlap at all, just butt one track against the next. */
  | 'gapless';

export interface EqAutomation {
  /** 0..1 multipliers at the start and end of the transition, per band. */
  outgoing: { lowFrom: number; lowTo: number; midFrom: number; midTo: number };
  incoming: { lowFrom: number; lowTo: number; midFrom: number; midTo: number };
}

export interface TransitionScore {
  tempo: number;
  beatAlignment: number;
  phraseAlignment: number;
  loudness: number;
  structure: number;
  total: number;
}

export interface TransitionPlan {
  style: TransitionStyle;

  /** Where the transition begins in the outgoing track, milliseconds. */
  outgoingStartMs: number;
  /** Where the outgoing track stops contributing. */
  outgoingEndMs: number;
  /** Where the incoming track begins playing from, milliseconds. */
  incomingStartMs: number;

  durationMs: number;

  /** Tempo both decks end up at. */
  targetBpm: number;
  /** Playback rate multipliers. 1.0 means untouched. */
  outgoingTempoRatio: number;
  incomingTempoRatio: number;

  /** Linear gain applied for loudness matching, not the crossfade itself. */
  outgoingGain: number;
  incomingGain: number;

  eq: EqAutomation;

  score: TransitionScore;

  /**
   * Present when the plan is not the ideal one, explaining what forced the
   * fallback. Surfaced in the debug view; never hidden.
   */
  fallbackReason?: string;
}

export interface TransitionOptions {
  length: TransitionLength;
  /** Prefer stretching the incoming track rather than the outgoing one. */
  adaptIncoming: boolean;
}

export const DEFAULT_TRANSITION_OPTIONS: TransitionOptions = {
  length: 'medium',
  // A DJ rides the incoming record onto the tempo of what is already playing;
  // changing the tempo of the track people are currently dancing to is the
  // more noticeable of the two.
  adaptIncoming: true,
};

/** Transition length in bars, before clamping to what the tracks allow. */
const LENGTH_IN_BARS: Record<TransitionLength, number> = {
  short: 4,
  medium: 8,
  long: 16,
};

/** Linear gain for a decibel change. */
export function decibelsToGain(decibels: number): number {
  return Math.pow(10, decibels / 20);
}

/**
 * Gain that moves a track towards the target loudness, clamped.
 * Returns 1.0 when loudness is unknown rather than guessing.
 */
export function loudnessGain(integratedLufs: number): number {
  if (!Number.isFinite(integratedLufs) || integratedLufs >= 0) return 1;
  const correction = Math.max(
    -MAX_GAIN_CORRECTION_DB,
    Math.min(MAX_GAIN_CORRECTION_DB, TARGET_LUFS - integratedLufs),
  );
  return decibelsToGain(correction);
}

/** Index of the beat nearest `timeMs`, or -1 when there is no grid. */
export function nearestBeatIndex(
  beatsMs: readonly number[],
  timeMs: number,
): number {
  if (beatsMs.length === 0) return -1;

  // Binary search: grids run to tens of thousands of entries on long mixes,
  // and this is called repeatedly while evaluating candidate points.
  let low = 0;
  let high = beatsMs.length - 1;
  while (low < high) {
    const mid = (low + high) >> 1;
    if ((beatsMs[mid] as number) < timeMs) low = mid + 1;
    else high = mid;
  }

  if (low > 0) {
    const previous = beatsMs[low - 1] as number;
    const current = beatsMs[low] as number;
    if (Math.abs(previous - timeMs) <= Math.abs(current - timeMs)) return low - 1;
  }
  return low;
}

/**
 * The downbeat at or after `timeMs`, preferring one that starts a phrase.
 *
 * Phrases in dance music run in multiples of four bars, and a transition that
 * begins mid-phrase feels early or late even when every beat lines up. Where
 * no phrase boundary is available in range, this falls back to any downbeat
 * rather than failing - a bar line is still far better than an arbitrary time.
 */
export function phraseAlignedDownbeat(
  track: TransitionTrack,
  timeMs: number,
  phraseBars = 4,
): { beatIndex: number; onPhrase: boolean } | null {
  if (track.downbeatIndices.length === 0) return null;

  let firstAtOrAfter = -1;
  for (let i = 0; i < track.downbeatIndices.length; i += 1) {
    const beatIndex = track.downbeatIndices[i] as number;
    const beatTime = track.beatsMs[beatIndex];
    if (beatTime === undefined) continue;
    if (beatTime >= timeMs) {
      firstAtOrAfter = i;
      break;
    }
  }

  if (firstAtOrAfter < 0) return null;

  // Prefer the next downbeat whose position in the bar sequence starts a
  // phrase, but do not wander more than one phrase away looking for it.
  for (let i = firstAtOrAfter; i < track.downbeatIndices.length; i += 1) {
    if (i - firstAtOrAfter >= phraseBars) break;
    if (i % phraseBars === 0) {
      return { beatIndex: track.downbeatIndices[i] as number, onPhrase: true };
    }
  }

  return {
    beatIndex: track.downbeatIndices[firstAtOrAfter] as number,
    onPhrase: false,
  };
}

/**
 * Where to bring a track in.
 *
 * Prefers the start of a calm section in the first part of the track over the
 * first audible sample. On material that opens with its hook - most edits and
 * mashups - cueing to the first sound drops a vocal straight on top of the
 * outgoing track's vocal, which is the single most obvious way a mix sounds
 * amateurish.
 *
 * Falls back to the first audible moment when there is no section quieter than
 * the track's own average, which is normal for a continuous DJ mix that starts
 * at full energy.
 */
export function chooseEntryPoint(track: TransitionTrack): number {
  if (track.sections.length === 0) return track.introEndMs;

  const averageEnergy =
    track.sections.reduce((sum, section) => sum + section.energy, 0) /
    track.sections.length;

  // Only consider the opening third: entering three quarters of the way into
  // a track is not an entry, it is skipping most of it.
  const searchLimit = track.introEndMs + (track.durationMs - track.introEndMs) / 3;

  let best: number | null = null;
  for (const section of track.sections) {
    if (section.startMs < track.introEndMs) continue;
    if (section.startMs > searchLimit) break;
    if (section.energy <= averageEnergy) {
      best = section.startMs;
      break;
    }
  }

  return best ?? track.introEndMs;
}

/**
 * Where to start mixing out of a track.
 *
 * Prefers a calm section late in the track - a breakdown or an outro - over an
 * arbitrary "N seconds before the end". Mixing out of a chorus is possible but
 * fights the music; mixing out of a quiet passage sounds deliberate.
 */
export function chooseExitPoint(
  track: TransitionTrack,
  transitionDurationMs: number,
): number {
  const latestSensible = Math.max(
    track.introEndMs,
    track.outroStartMs - transitionDurationMs,
  );

  if (track.sections.length === 0) return latestSensible;

  const averageEnergy =
    track.sections.reduce((sum, section) => sum + section.energy, 0) /
    track.sections.length;

  // Search backwards for a calm section that still leaves room for the whole
  // transition before the track runs out.
  let best: number | null = null;
  for (let i = track.sections.length - 1; i >= 0; i -= 1) {
    const section = track.sections[i];
    if (section === undefined) continue;
    if (section.startMs > latestSensible) continue;
    // Not so early that most of the track is thrown away.
    if (section.startMs < track.durationMs / 2) break;
    if (section.energy <= averageEnergy) {
      best = section.startMs;
      break;
    }
  }

  return best ?? latestSensible;
}

function hasUsableGrid(track: TransitionTrack): boolean {
  return (
    track.beatsMs.length >= 8 &&
    track.bpm > 0 &&
    track.beatConfidence >= MIN_BEAT_CONFIDENCE
  );
}

/** Neutral EQ: everything open, nothing automated. */
function flatEq(): EqAutomation {
  return {
    outgoing: { lowFrom: 1, lowTo: 1, midFrom: 1, midTo: 1 },
    incoming: { lowFrom: 1, lowTo: 1, midFrom: 1, midTo: 1 },
  };
}

/**
 * The classic bass swap.
 *
 * Two kick drums and two basslines playing at once is the single muddiest
 * thing that can happen in a mix, even when they are perfectly beat-matched -
 * low frequencies carry most of the energy, so they sum into a boomy mess.
 * So the outgoing bass is pulled out while the incoming bass comes in, and
 * the mids follow more gently. Highs are left to the crossfade, since hats
 * and air layer together without trouble.
 */
function bassSwapEq(): EqAutomation {
  return {
    outgoing: { lowFrom: 1, lowTo: 0, midFrom: 1, midTo: 0.6 },
    incoming: { lowFrom: 0, lowTo: 1, midFrom: 0.6, midTo: 1 },
  };
}

/**
 * Plans the move from `outgoing` into `incoming`.
 *
 * Always returns a plan. There is no failure mode where playback stops: the
 * ladder degrades from phrase-aligned, to beat-aligned, to a plain crossfade,
 * to gapless, and every step down records why.
 */
export function planTransition(
  outgoing: TransitionTrack,
  incoming: TransitionTrack,
  options: TransitionOptions = DEFAULT_TRANSITION_OPTIONS,
): TransitionPlan {
  const outgoingGain = loudnessGain(outgoing.integratedLufs);
  const incomingGain = loudnessGain(incoming.integratedLufs);

  const loudnessGapDb = Math.abs(
    outgoing.integratedLufs - incoming.integratedLufs,
  );
  const loudnessScore = Math.max(0, 1 - loudnessGapDb / 12);

  // --- Can we beat-match at all? ---------------------------------------
  const outgoingUsable = hasUsableGrid(outgoing);
  const incomingUsable = hasUsableGrid(incoming);

  if (!outgoingUsable || !incomingUsable) {
    const which = !outgoingUsable && !incomingUsable
      ? 'Neither track has'
      : !outgoingUsable
        ? `"${outgoing.title}" does not have`
        : `"${incoming.title}" does not have`;
    return simpleCrossfade(
      outgoing,
      incoming,
      options,
      outgoingGain,
      incomingGain,
      loudnessScore,
      `${which} a beat grid reliable enough to match against.`,
    );
  }

  // --- Tempo ------------------------------------------------------------
  // Try the incoming track at half and double time too: 140 into 70 is the
  // same pulse at another metrical level, not a 100% tempo change.
  const candidates = [incoming.bpm, incoming.bpm * 2, incoming.bpm / 2];
  let bestBpm = incoming.bpm;
  let bestDeviation = Number.POSITIVE_INFINITY;
  for (const candidate of candidates) {
    const deviation = Math.abs(Math.log2(candidate / outgoing.bpm));
    if (deviation < bestDeviation) {
      bestDeviation = deviation;
      bestBpm = candidate;
    }
  }

  const targetBpm = outgoing.bpm;
  const requiredRatio = targetBpm / bestBpm;
  const stretch = Math.abs(requiredRatio - 1);

  if (stretch > MAX_TEMPO_STRETCH) {
    return simpleCrossfade(
      outgoing,
      incoming,
      options,
      outgoingGain,
      incomingGain,
      loudnessScore,
      `Tempos are ${(stretch * 100).toFixed(0)}% apart, beyond the ` +
        `${(MAX_TEMPO_STRETCH * 100).toFixed(0)}% a stretch can hide.`,
    );
  }

  const incomingTempoRatio = options.adaptIncoming ? requiredRatio : 1;
  const outgoingTempoRatio = options.adaptIncoming ? 1 : bestBpm / outgoing.bpm;

  // --- Where to leave the outgoing track --------------------------------
  const bars = LENGTH_IN_BARS[options.length];
  const msPerBar = (60000 / targetBpm) * outgoing.beatsPerBar;
  const wantedDurationMs = bars * msPerBar;

  // Pick musical points first, then snap each to a downbeat. Doing it in this
  // order matters: snapping an arbitrary timestamp to a bar line gives a
  // rhythmically tidy transition in a musically wrong place.
  const idealStart = chooseExitPoint(outgoing, wantedDurationMs);
  const idealEntry = chooseEntryPoint(incoming);

  const outgoingPoint = phraseAlignedDownbeat(outgoing, idealStart);
  const incomingPoint = phraseAlignedDownbeat(incoming, idealEntry);

  if (outgoingPoint === null || incomingPoint === null) {
    return simpleCrossfade(
      outgoing,
      incoming,
      options,
      outgoingGain,
      incomingGain,
      loudnessScore,
      'No downbeats were detected, so bars cannot be lined up.',
    );
  }

  const outgoingStartMs = outgoing.beatsMs[outgoingPoint.beatIndex] as number;
  const incomingStartMs = incoming.beatsMs[incomingPoint.beatIndex] as number;

  // Never run past the end of either track.
  const remainingOutgoing = outgoing.durationMs - outgoingStartMs;
  const remainingIncoming =
    (incoming.durationMs - incomingStartMs) / incomingTempoRatio;
  const durationMs = Math.max(
    msPerBar,
    Math.min(wantedDurationMs, remainingOutgoing, remainingIncoming),
  );

  const onPhrase = outgoingPoint.onPhrase && incomingPoint.onPhrase;

  const tempoScore = 1 - stretch / MAX_TEMPO_STRETCH;
  const beatScore = Math.min(outgoing.beatConfidence, incoming.beatConfidence);
  const phraseScore = onPhrase ? 1 : 0.5;
  // Mixing out of an outro and into an intro is what a DJ aims for.
  const structureScore =
    (outgoingStartMs >= outgoing.outroStartMs - wantedDurationMs ? 0.5 : 0.2) +
    (incomingStartMs <= incoming.introEndMs + wantedDurationMs ? 0.5 : 0.2);

  return {
    style: onPhrase ? 'phraseAligned' : 'beatAligned',
    outgoingStartMs,
    outgoingEndMs: outgoingStartMs + durationMs,
    incomingStartMs,
    durationMs,
    targetBpm,
    outgoingTempoRatio,
    incomingTempoRatio,
    outgoingGain,
    incomingGain,
    eq: bassSwapEq(),
    score: {
      tempo: tempoScore,
      beatAlignment: beatScore,
      phraseAlignment: phraseScore,
      loudness: loudnessScore,
      structure: structureScore,
      total:
        tempoScore * 0.3 +
        beatScore * 0.25 +
        phraseScore * 0.2 +
        loudnessScore * 0.15 +
        structureScore * 0.1,
    },
  };
}

/**
 * Fallback: overlap without beat-matching.
 *
 * Deliberately longer than a beat-matched transition and with no bass swap.
 * Without a shared grid the two tracks will drift against each other, and a
 * slow blend of unrelated material reads as a deliberate mix, whereas a short
 * one reads as a mistake. Leaving both basslines alone is also safer here:
 * ducking the bass of a track that is not rhythmically locked just sounds like
 * the sound is broken.
 */
function simpleCrossfade(
  outgoing: TransitionTrack,
  incoming: TransitionTrack,
  options: TransitionOptions,
  outgoingGain: number,
  incomingGain: number,
  loudnessScore: number,
  reason: string,
): TransitionPlan {
  const wantedDurationMs =
    options.length === 'short' ? 3000 : options.length === 'long' ? 10000 : 6000;

  const outgoingStartMs = Math.max(
    0,
    Math.min(
      chooseExitPoint(outgoing, wantedDurationMs),
      outgoing.durationMs - wantedDurationMs,
    ),
  );
  // Even without a grid, entering on a calm section beats entering on the hook.
  const incomingStartMs = Math.max(0, chooseEntryPoint(incoming));

  const durationMs = Math.max(
    500,
    Math.min(
      wantedDurationMs,
      outgoing.durationMs - outgoingStartMs,
      incoming.durationMs - incomingStartMs,
    ),
  );

  return {
    style: 'crossfade',
    outgoingStartMs,
    outgoingEndMs: outgoingStartMs + durationMs,
    incomingStartMs,
    durationMs,
    targetBpm: outgoing.bpm > 0 ? outgoing.bpm : incoming.bpm,
    outgoingTempoRatio: 1,
    incomingTempoRatio: 1,
    outgoingGain,
    incomingGain,
    eq: flatEq(),
    score: {
      tempo: 0,
      beatAlignment: 0,
      phraseAlignment: 0,
      loudness: loudnessScore,
      structure: 0.5,
      total: loudnessScore * 0.15 + 0.05,
    },
    fallbackReason: reason,
  };
}

/**
 * Equal-power crossfade gains at a point through the transition.
 *
 * Equal power, not linear: two uncorrelated signals sum in power, so a linear
 * fade dips about 3 dB in the middle - an audible sag exactly where the two
 * tracks are supposed to be strongest together.
 */
export function crossfadeGains(progress: number): {
  outgoing: number;
  incoming: number;
} {
  const t = Math.max(0, Math.min(1, progress));
  return {
    outgoing: Math.cos((t * Math.PI) / 2),
    incoming: Math.sin((t * Math.PI) / 2),
  };
}

/** Human-readable summary for the debug view. */
export function describeTransition(plan: TransitionPlan): string {
  if (plan.fallbackReason !== undefined) {
    return `${plan.style}: ${plan.fallbackReason}`;
  }
  const stretchPercent = Math.abs(plan.incomingTempoRatio - 1) * 100;
  const bars = (plan.durationMs / ((60000 / plan.targetBpm) * 4)).toFixed(1);
  return (
    `${plan.style} at ${plan.targetBpm.toFixed(1)} BPM, ${bars} bars` +
    (stretchPercent > 0.05 ? `, incoming stretched ${stretchPercent.toFixed(1)}%` : '')
  );
}
