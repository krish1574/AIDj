import { describe, expect, it } from 'vitest';

import {
  DEFAULT_SESSION_OPTIONS,
  PREPARE_LEAD_MS,
  buildSession,
  msUntilTransition,
  shouldPrepareNext,
  type SessionTrack,
} from './session';

function sessionTrack(
  overrides: Partial<SessionTrack> & { id: number },
): SessionTrack {
  const bpm = overrides.bpm ?? 128;
  const durationMs = overrides.durationMs ?? 240_000;
  const beatInterval = 60_000 / bpm;
  const beatCount = Math.floor(durationMs / beatInterval);

  return {
    title: `Track ${overrides.id}`,
    contentUri: `content://media/${overrides.id}`,
    durationMs,
    bpm,
    bpmConfidence: 0.8,
    alternateBpm: bpm / 2,
    beatConfidence: 0.8,
    beatsMs: Array.from({ length: beatCount }, (_, i) => i * beatInterval),
    downbeatIndices: Array.from({ length: Math.floor(beatCount / 4) }, (_, i) => i * 4),
    beatsPerBar: 4,
    introEndMs: 2_000,
    outroStartMs: durationMs - 20_000,
    integratedLufs: -10,
    sections: [],
    energy: 0.5,
    key: { tonic: 0, mode: 'major' },
    keyConfidence: 0.6,
    vocalActivity: 0.3,
    ...overrides,
  };
}

describe('buildSession', () => {
  const playlist = () => [
    sessionTrack({ id: 1, bpm: 128 }),
    sessionTrack({ id: 2, bpm: 127 }),
    sessionTrack({ id: 3, bpm: 129 }),
    sessionTrack({ id: 4, bpm: 126 }),
  ];

  it('plays every track exactly once', () => {
    const steps = buildSession(playlist());
    expect(steps).toHaveLength(4);
    expect(steps.map((step) => step.track.id).sort()).toEqual([1, 2, 3, 4]);
  });

  it('gives the opener no transition and everything else one', () => {
    const steps = buildSession(playlist());
    expect(steps[0]?.transition).toBeNull();
    for (const step of steps.slice(1)) {
      expect(step.transition).not.toBeNull();
    }
  });

  it('alternates voices so the next track always has a free one', () => {
    // Two voices is the constraint the whole preparation pipeline lives under:
    // a track can only be prepared once the voice it needs has been released
    // by the track two before it.
    const steps = buildSession(playlist());
    for (const [index, step] of steps.entries()) {
      expect(step.voice).toBe(index % 2);
      const previous = steps[index - 1];
      if (previous !== undefined) expect(step.voice).not.toBe(previous.voice);
    }
  });

  it('plans each transition between the tracks that actually adjoin', () => {
    // The queue planner may reorder, so a transition must be planned against
    // the track that ends up before it, not the one that started before it.
    const steps = buildSession(playlist());
    for (const [index, step] of steps.entries()) {
      if (index === 0 || step.transition === null) continue;
      const previous = steps[index - 1];
      expect(previous).toBeDefined();
      // The plan must fit inside the outgoing track it was planned against.
      expect(step.transition.outgoingStartMs).toBeLessThanOrEqual(
        (previous as { track: SessionTrack }).track.durationMs,
      );
    }
  });

  it('preserves playlist order when reordering is disabled', () => {
    const steps = buildSession(playlist(), {
      ...DEFAULT_SESSION_OPTIONS,
      allowReordering: false,
    });
    expect(steps.map((step) => step.track.id)).toEqual([1, 2, 3, 4]);
  });

  it('handles a single track and an empty playlist', () => {
    expect(buildSession([])).toEqual([]);

    const single = buildSession([sessionTrack({ id: 1 })]);
    expect(single).toHaveLength(1);
    expect(single[0]?.transition).toBeNull();
  });

  it('always produces a playable step even for unanalysable tracks', () => {
    // A party must not stop because one file could not be analysed.
    const broken = sessionTrack({
      id: 1,
      bpm: 0,
      beatsMs: [],
      downbeatIndices: [],
      beatConfidence: 0,
    });
    const steps = buildSession([broken, { ...broken, id: 2 }]);

    expect(steps).toHaveLength(2);
    expect(steps[1]?.transition?.durationMs).toBeGreaterThan(0);
    expect(steps[1]?.transition?.fallbackReason).toBeDefined();
  });
});

describe('transition timing', () => {
  const steps = buildSession([
    sessionTrack({ id: 1, bpm: 128 }),
    sessionTrack({ id: 2, bpm: 128 }),
  ]);
  const transition = steps[1]?.transition;

  it('reports time remaining until the transition point', () => {
    expect(transition).not.toBeNull();
    if (transition == null) return;

    const atStart = msUntilTransition(0, transition);
    expect(atStart).toBeCloseTo(transition.outgoingStartMs, 5);

    const halfway = msUntilTransition(transition.outgoingStartMs / 2, transition);
    expect(halfway).toBeLessThan(atStart);
  });

  it('reports negative once the moment has passed', () => {
    if (transition == null) return;
    // The caller treats this as "fire now": a late transition is recoverable,
    // a missed one is a silence.
    expect(msUntilTransition(transition.outgoingStartMs + 5_000, transition))
      .toBeLessThan(0);
  });

  it('asks for preparation with enough lead time', () => {
    if (transition == null) return;

    const early = transition.outgoingStartMs - PREPARE_LEAD_MS - 1_000;
    expect(shouldPrepareNext(early, transition)).toBe(false);

    const late = transition.outgoingStartMs - PREPARE_LEAD_MS + 1_000;
    expect(shouldPrepareNext(late, transition)).toBe(true);
  });

  it('still asks for preparation if the moment has already passed', () => {
    if (transition == null) return;
    expect(shouldPrepareNext(transition.outgoingStartMs + 1_000, transition))
      .toBe(true);
  });
});
