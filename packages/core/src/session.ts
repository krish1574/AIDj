import { type PlannerOptions, MIX_STYLES, planQueue, type PlannerTrack } from './planner';
import type { MixStyle } from './preferences';
import {
  DEFAULT_TRANSITION_OPTIONS,
  planTransition,
  type TransitionOptions,
  type TransitionPlan,
  type TransitionTrack,
} from './transition';

/**
 * The continuous DJ session.
 *
 * This is the product: a playlist goes in, and music comes out continuously
 * with no further input. Everything before this - analysis, queue planning,
 * transition planning - exists to make this possible, and none of it is
 * useful on its own.
 *
 * The logic here is deliberately pure. Deciding what plays next, when to
 * prepare it, and when to fire a transition are all decisions that can be
 * wrong in ways that are hard to reproduce on a device, so they are testable
 * without one. The hook that drives the audio engine contains no decisions.
 */

/** A track with everything both planners need, plus how to play it. */
export interface SessionTrack extends TransitionTrack {
  contentUri: string;
  energy: number;
  bpmConfidence: number;
  alternateBpm: number;
  keyConfidence: number;
  key: PlannerTrack['key'];
  vocalActivity: number;
}

export interface SessionStep {
  track: SessionTrack;
  /**
   * How this track is mixed in from the one before it. Null for the opener,
   * which has nothing to mix from.
   */
  transition: TransitionPlan | null;
  /**
   * Which engine voice plays it. Two voices alternate, which is why the
   * preparation pipeline can only ever look one track ahead: the voice the
   * track after next would need is still playing.
   */
  voice: 0 | 1;
}

export interface SessionOptions {
  mixStyle: MixStyle;
  transition: TransitionOptions;
  /** When false, the user's playlist order is preserved exactly. */
  allowReordering: boolean;
}

export const DEFAULT_SESSION_OPTIONS: SessionOptions = {
  mixStyle: 'balanced',
  transition: DEFAULT_TRANSITION_OPTIONS,
  allowReordering: true,
};

/**
 * Turns a playlist into a fully planned session.
 *
 * Both planners run here: the queue planner decides the order, then the
 * transition planner works out each join. Doing it up front rather than
 * one-at-a-time means the whole set can be inspected and shown to the user
 * before a note plays, and it makes the result reproducible.
 */
export function buildSession(
  tracks: readonly SessionTrack[],
  options: SessionOptions = DEFAULT_SESSION_OPTIONS,
): SessionStep[] {
  if (tracks.length === 0) return [];

  const plannerOptions: PlannerOptions = {
    ...MIX_STYLES[options.mixStyle],
    allowReordering: options.allowReordering,
  };

  const byId = new Map(tracks.map((track) => [track.id, track]));

  const plannerTracks: PlannerTrack[] = tracks.map((track, index) => ({
    id: track.id,
    title: track.title,
    originalIndex: index,
    durationMs: track.durationMs,
    bpm: track.bpm,
    bpmConfidence: track.bpmConfidence,
    alternateBpm: track.alternateBpm,
    key: track.key,
    keyConfidence: track.keyConfidence,
    energy: track.energy,
    integratedLufs: track.integratedLufs,
    vocalActivity: track.vocalActivity,
  }));

  const queue = planQueue(plannerTracks, plannerOptions);

  const steps: SessionStep[] = [];
  for (const [index, item] of queue.entries()) {
    const track = byId.get(item.track.id);
    if (track === undefined) continue;

    const previous = steps[steps.length - 1]?.track;
    steps.push({
      track,
      transition:
        previous === undefined
          ? null
          : planTransition(previous, track, options.transition),
      // Alternating: the voice a track needs is free because the track two
      // back has finished by the time this one starts.
      voice: (steps.length % 2) as 0 | 1,
    });
  }

  return steps;
}

/**
 * How long until the transition into `step` should be armed.
 *
 * `positionMs` is the current playhead within the outgoing track, in that
 * track's own timeline. Negative means the moment has passed, which the caller
 * should treat as "fire immediately" rather than as an error - a late
 * transition is recoverable, a missed one is a gap.
 */
export function msUntilTransition(
  positionMs: number,
  transition: TransitionPlan,
): number {
  return transition.outgoingStartMs - positionMs;
}

/**
 * Whether the next track should be decoding by now.
 *
 * Preparation has to start far enough ahead that a slow file, a cold cache or
 * a throttled CPU cannot make the transition wait on I/O. Twenty seconds is
 * generous on purpose: the cost of preparing early is one extra decoder
 * running, and the cost of preparing late is a silence in the middle of a
 * party.
 */
export const PREPARE_LEAD_MS = 20_000;

export function shouldPrepareNext(
  positionMs: number,
  transition: TransitionPlan,
): boolean {
  return msUntilTransition(positionMs, transition) <= PREPARE_LEAD_MS;
}

/** Total playing time of a session, accounting for transition overlaps. */
export function sessionDurationMs(steps: readonly SessionStep[]): number {
  if (steps.length === 0) return 0;

  let total = 0;
  for (const [index, step] of steps.entries()) {
    const next = steps[index + 1];
    if (next?.transition == null) {
      // Last track, or one with no planned join: it plays to its end.
      total += step.track.durationMs - (step.transition?.incomingStartMs ?? 0);
      continue;
    }
    // This track is audible from where it was cued until the next transition
    // begins; the overlap belongs to the next step.
    total +=
      next.transition.outgoingStartMs - (step.transition?.incomingStartMs ?? 0);
  }
  return Math.max(0, total);
}

/** One-line summary of a step for the queue UI. */
export function describeStep(step: SessionStep): string {
  if (step.transition === null) return 'Opens the set';
  if (step.transition.fallbackReason !== undefined) {
    return `Crossfade - ${step.transition.fallbackReason}`;
  }
  const stretch = Math.abs(step.transition.incomingTempoRatio - 1) * 100;
  return (
    `${step.transition.style} at ${step.transition.targetBpm.toFixed(0)} BPM` +
    (stretch > 0.05 ? `, stretched ${stretch.toFixed(1)}%` : '')
  );
}
