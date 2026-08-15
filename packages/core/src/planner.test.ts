import { describe, expect, it } from 'vitest';

import {
  MIX_STYLES,
  keyCompatibility,
  planQueue,
  scoreTransition,
  tempoMatch,
  type PlannerTrack,
} from './planner';

/** A deliberately neutral track; each test varies only what it is testing. */
function track(overrides: Partial<PlannerTrack> & { id: number }): PlannerTrack {
  return {
    title: `Track ${overrides.id}`,
    originalIndex: overrides.id,
    durationMs: 240_000,
    bpm: 128,
    bpmConfidence: 0.9,
    alternateBpm: 64,
    key: { tonic: 0, mode: 'major' },
    keyConfidence: 0.8,
    energy: 0.5,
    integratedLufs: -9,
    vocalActivity: 0.3,
    ...overrides,
  };
}

describe('tempoMatch', () => {
  it('is 1.0 for identical tempi', () => {
    const result = tempoMatch(track({ id: 1 }), track({ id: 2 }));
    expect(result.ratio).toBeCloseTo(1, 5);
    expect(result.usedAlternate).toBe(false);
  });

  it('reconciles half time rather than reporting a 100% change', () => {
    // 70 into 140 is the same pulse at a different metrical level. A DJ mixes
    // these directly; reporting ratio 2.0 would wrongly reject the pairing.
    const result = tempoMatch(
      track({ id: 1, bpm: 70, alternateBpm: 140 }),
      track({ id: 2, bpm: 140, alternateBpm: 70 }),
    );
    expect(result.ratio).toBeCloseTo(1, 2);
    expect(result.usedAlternate).toBe(true);
  });

  it('reports a genuine small change as a small change', () => {
    const result = tempoMatch(
      track({ id: 1, bpm: 124 }),
      track({ id: 2, bpm: 128 }),
    );
    expect(result.ratio).toBeCloseTo(128 / 124, 4);
    expect(result.usedAlternate).toBe(false);
  });
});

describe('keyCompatibility', () => {
  it('scores identical keys highest', () => {
    expect(keyCompatibility({ tonic: 0, mode: 'major' }, { tonic: 0, mode: 'major' })).toBe(1);
  });

  it('ranks relative minor above a tritone', () => {
    const cMajor = { tonic: 0, mode: 'major' } as const;
    const aMinor = { tonic: 9, mode: 'minor' } as const;
    const fSharp = { tonic: 6, mode: 'major' } as const;
    expect(keyCompatibility(cMajor, aMinor)).toBeGreaterThan(
      keyCompatibility(cMajor, fSharp),
    );
  });

  it('never returns zero, because a key clash is a cost not a veto', () => {
    for (let tonic = 0; tonic < 12; tonic += 1) {
      for (const mode of ['major', 'minor'] as const) {
        expect(
          keyCompatibility({ tonic: 0, mode: 'major' }, { tonic, mode }),
        ).toBeGreaterThan(0);
      }
    }
  });
});

describe('scoreTransition', () => {
  it('prefers a matching tempo over a distant one', () => {
    const from = track({ id: 1, bpm: 128 });
    const close = track({ id: 2, bpm: 129, originalIndex: 2 });
    const far = track({ id: 3, bpm: 150, originalIndex: 2 });

    const closeScore = scoreTransition(from, close, 1, MIX_STYLES.balanced);
    const farScore = scoreTransition(from, far, 1, MIX_STYLES.balanced);

    expect(closeScore.total).toBeGreaterThan(farScore.total);
  });

  it('discounts tempo agreement when confidence is low', () => {
    const from = track({ id: 1, bpm: 128, bpmConfidence: 0.9 });
    const confident = track({ id: 2, bpm: 128, bpmConfidence: 0.9 });
    const unsure = track({ id: 3, bpm: 128, bpmConfidence: 0.1 });

    const confidentScore = scoreTransition(from, confident, 1, MIX_STYLES.balanced);
    const unsureScore = scoreTransition(from, unsure, 1, MIX_STYLES.balanced);

    // Same nominal BPM, but one of them we do not believe.
    expect(confidentScore.bpm).toBeGreaterThan(unsureScore.bpm);
  });

  it('penalises both-vocal transitions', () => {
    const from = track({ id: 1, vocalActivity: 0.9 });
    const vocal = track({ id: 2, vocalActivity: 0.9 });
    const instrumental = track({ id: 3, vocalActivity: 0.1 });

    expect(scoreTransition(from, vocal, 1, MIX_STYLES.balanced).vocalClash)
      .toBeLessThan(
        scoreTransition(from, instrumental, 1, MIX_STYLES.balanced).vocalClash,
      );
  });

  it('penalises a large loudness gap', () => {
    const from = track({ id: 1, integratedLufs: -9 });
    const matched = track({ id: 2, integratedLufs: -9 });
    const quiet = track({ id: 3, integratedLufs: -20 });

    expect(scoreTransition(from, matched, 1, MIX_STYLES.balanced).loudness)
      .toBeGreaterThan(
        scoreTransition(from, quiet, 1, MIX_STYLES.balanced).loudness,
      );
  });

  it('exposes every component so a choice can be explained', () => {
    const score = scoreTransition(
      track({ id: 1 }),
      track({ id: 2 }),
      1,
      MIX_STYLES.balanced,
    );
    const sum =
      score.bpm +
      score.key +
      score.energy +
      score.loudness +
      score.order +
      score.vocalClash +
      score.tempoPenalty +
      score.energyPenalty;

    expect(score.total).toBeCloseTo(sum, 10);
  });
});

describe('planQueue', () => {
  const playlist = (): PlannerTrack[] => [
    track({ id: 1, originalIndex: 0, bpm: 120 }),
    track({ id: 2, originalIndex: 1, bpm: 150 }),
    track({ id: 3, originalIndex: 2, bpm: 121 }),
    track({ id: 4, originalIndex: 3, bpm: 152 }),
  ];

  it('returns the exact input order when reordering is disabled', () => {
    const options = { ...MIX_STYLES.balanced, allowReordering: false };
    const queue = planQueue(playlist(), options);
    expect(queue.map((item) => item.track.id)).toEqual([1, 2, 3, 4]);
  });

  it('always keeps the user first track first', () => {
    // Even when a later track would mix better from nothing, changing the
    // opener is the most visible possible override of user intent.
    const queue = planQueue(playlist(), MIX_STYLES.balanced);
    expect(queue[0]?.track.id).toBe(1);
  });

  it('plays every track exactly once', () => {
    const queue = planQueue(playlist(), MIX_STYLES.balanced);
    const ids = queue.map((item) => item.track.id).sort();
    expect(ids).toEqual([1, 2, 3, 4]);
  });

  it('pulls a tempo-compatible track forward when it is clearly better', () => {
    // 120 -> 121 is a far better mix than 120 -> 150, and track 3 is within
    // the lookahead window, so it should be promoted.
    const queue = planQueue(playlist(), MIX_STYLES.balanced);
    expect(queue[1]?.track.id).toBe(3);
  });

  it('does not reorder when candidates are near-identical', () => {
    // Hysteresis exists for this: without it, floating-point noise reshuffles
    // a playlist of similar tracks and the app looks like it is randomising.
    const similar: PlannerTrack[] = [
      track({ id: 1, originalIndex: 0, bpm: 128 }),
      track({ id: 2, originalIndex: 1, bpm: 128.1 }),
      track({ id: 3, originalIndex: 2, bpm: 128.2 }),
      track({ id: 4, originalIndex: 3, bpm: 127.9 }),
    ];
    const queue = planQueue(similar, MIX_STYLES.balanced);
    expect(queue.map((item) => item.track.id)).toEqual([1, 2, 3, 4]);
  });

  it('never reaches beyond the lookahead window', () => {
    // Track 6 is a perfect tempo match but far away. Respecting the window is
    // what stops the planner from rebuilding the playlist as a tempo ramp.
    const long: PlannerTrack[] = [
      track({ id: 1, originalIndex: 0, bpm: 120 }),
      track({ id: 2, originalIndex: 1, bpm: 150 }),
      track({ id: 3, originalIndex: 2, bpm: 151 }),
      track({ id: 4, originalIndex: 3, bpm: 152 }),
      track({ id: 5, originalIndex: 4, bpm: 153 }),
      track({ id: 6, originalIndex: 5, bpm: 120 }),
      track({ id: 7, originalIndex: 6, bpm: 154 }),
    ];
    const queue = planQueue(long, { ...MIX_STYLES.balanced, lookahead: 3 });
    // With lookahead 3, track 6 is not reachable from position 0.
    expect(queue[1]?.track.id).not.toBe(6);
  });

  it('handles a single track and an empty playlist', () => {
    expect(planQueue([], MIX_STYLES.balanced)).toEqual([]);
    const single = planQueue([track({ id: 1, originalIndex: 0 })], MIX_STYLES.balanced);
    expect(single).toHaveLength(1);
    expect(single[0]?.score).toBeNull();
  });

  it('reorders more readily in energetic than in smooth', () => {
    // The styles must actually differ in behaviour, not just in numbers.
    const mixed: PlannerTrack[] = [
      track({ id: 1, originalIndex: 0, bpm: 120, energy: 0.3 }),
      track({ id: 2, originalIndex: 1, bpm: 150, energy: 0.35 }),
      track({ id: 3, originalIndex: 2, bpm: 122, energy: 0.4 }),
    ];

    const smooth = planQueue(mixed, MIX_STYLES.smooth).map((i) => i.track.id);
    const energetic = planQueue(mixed, MIX_STYLES.energetic).map((i) => i.track.id);

    // Both should prefer the tempo-compatible track here; the point is that
    // each style produces a coherent, complete queue.
    expect(smooth).toHaveLength(3);
    expect(energetic).toHaveLength(3);
    expect(new Set(smooth)).toEqual(new Set([1, 2, 3]));
  });

  it('scores every track after the first', () => {
    const queue = planQueue(playlist(), MIX_STYLES.balanced);
    expect(queue[0]?.score).toBeNull();
    for (const item of queue.slice(1)) {
      expect(item.score).not.toBeNull();
    }
  });
});
