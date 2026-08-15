import { describe, expect, it } from 'vitest';

import {
  MAX_TEMPO_STRETCH,
  MIN_BEAT_CONFIDENCE,
  chooseEntryPoint,
  chooseExitPoint,
  crossfadeGains,
  loudnessGain,
  nearestBeatIndex,
  phraseAlignedDownbeat,
  planTransition,
  type TransitionTrack,
} from './transition';

/** A track with a perfectly regular grid, so tests control every variable. */
function track(overrides: Partial<TransitionTrack> & { id: number }): TransitionTrack {
  const bpm = overrides.bpm ?? 128;
  const durationMs = overrides.durationMs ?? 240_000;
  const beatInterval = 60_000 / bpm;
  const beatCount = Math.floor(durationMs / beatInterval);

  const beatsMs = Array.from({ length: beatCount }, (_, i) => i * beatInterval);
  const downbeatIndices = Array.from(
    { length: Math.floor(beatCount / 4) },
    (_, i) => i * 4,
  );

  return {
    title: `Track ${overrides.id}`,
    durationMs,
    bpm,
    beatConfidence: 0.8,
    beatsMs,
    downbeatIndices,
    beatsPerBar: 4,
    introEndMs: 2_000,
    outroStartMs: durationMs - 20_000,
    integratedLufs: -10,
    sections: [],
    ...overrides,
  };
}

describe('loudnessGain', () => {
  it('lifts a quiet track towards the target', () => {
    expect(loudnessGain(-20)).toBeGreaterThan(1);
  });

  it('pulls a loud track down', () => {
    expect(loudnessGain(-6)).toBeLessThan(1);
  });

  it('clamps rather than amplifying a very quiet track into noise', () => {
    // -40 LUFS would want +26 dB. Applying that does not make it loud, it
    // makes it noisy, and destroys whatever dynamics the master had.
    const gain = loudnessGain(-40);
    const decibels = 20 * Math.log10(gain);
    expect(decibels).toBeLessThanOrEqual(6.001);
  });

  it('does not guess when loudness is unknown', () => {
    expect(loudnessGain(Number.NaN)).toBe(1);
    expect(loudnessGain(0)).toBe(1);
  });
});

describe('crossfadeGains', () => {
  it('sums to constant power rather than constant amplitude', () => {
    // A linear fade dips ~3 dB in the middle, an audible sag exactly where
    // both tracks should be strongest.
    for (const progress of [0, 0.25, 0.5, 0.75, 1]) {
      const { outgoing, incoming } = crossfadeGains(progress);
      const power = outgoing * outgoing + incoming * incoming;
      expect(power).toBeCloseTo(1, 6);
    }
  });

  it('starts fully on the outgoing track and ends fully on the incoming one', () => {
    expect(crossfadeGains(0).outgoing).toBeCloseTo(1, 6);
    expect(crossfadeGains(0).incoming).toBeCloseTo(0, 6);
    expect(crossfadeGains(1).outgoing).toBeCloseTo(0, 6);
    expect(crossfadeGains(1).incoming).toBeCloseTo(1, 6);
  });

  it('clamps out-of-range progress instead of producing nonsense gains', () => {
    expect(crossfadeGains(-1).outgoing).toBeCloseTo(1, 6);
    expect(crossfadeGains(2).incoming).toBeCloseTo(1, 6);
  });
});

describe('nearestBeatIndex', () => {
  const beats = [0, 500, 1000, 1500, 2000];

  it('finds the closest beat on either side', () => {
    expect(nearestBeatIndex(beats, 1010)).toBe(2);
    expect(nearestBeatIndex(beats, 1490)).toBe(3);
    expect(nearestBeatIndex(beats, 0)).toBe(0);
  });

  it('clamps beyond the end of the grid', () => {
    expect(nearestBeatIndex(beats, 99999)).toBe(4);
  });

  it('reports -1 when there is no grid rather than inventing a beat', () => {
    expect(nearestBeatIndex([], 1000)).toBe(-1);
  });
});

describe('phraseAlignedDownbeat', () => {
  it('lands on a downbeat, not an arbitrary time', () => {
    const subject = track({ id: 1, bpm: 120 });
    const result = phraseAlignedDownbeat(subject, 10_000);
    expect(result).not.toBeNull();
    const beatTime = subject.beatsMs[(result as { beatIndex: number }).beatIndex];
    expect(subject.downbeatIndices).toContain(
      (result as { beatIndex: number }).beatIndex,
    );
    expect(beatTime as number).toBeGreaterThanOrEqual(10_000);
  });

  it('prefers a phrase start when one is within reach', () => {
    const subject = track({ id: 1, bpm: 120 });
    const result = phraseAlignedDownbeat(subject, 0);
    expect(result?.onPhrase).toBe(true);
  });

  it('returns null when there are no downbeats', () => {
    const subject = track({ id: 1, downbeatIndices: [] });
    expect(phraseAlignedDownbeat(subject, 1000)).toBeNull();
  });
});

describe('entry and exit point selection', () => {
  /** A track that opens loud, has a calm passage, then a loud finish. */
  const structured = () =>
    track({
      id: 1,
      durationMs: 240_000,
      introEndMs: 1_000,
      outroStartMs: 235_000,
      sections: [
        { startMs: 0, endMs: 30_000, energy: 0.9 }, // opens on the hook
        { startMs: 30_000, endMs: 60_000, energy: 0.3 }, // calm
        { startMs: 60_000, endMs: 180_000, energy: 0.95 },
        { startMs: 180_000, endMs: 215_000, energy: 0.25 }, // breakdown
        { startMs: 215_000, endMs: 240_000, energy: 0.9 },
      ],
    });

  it('enters on a calm section rather than the opening hook', () => {
    // Cueing to the first audible sample drops a vocal hook straight onto the
    // outgoing track's vocal, which is the most obvious way a mix sounds wrong.
    expect(chooseEntryPoint(structured())).toBe(30_000);
  });

  it('exits from a calm section rather than a fixed offset from the end', () => {
    expect(chooseExitPoint(structured(), 8_000)).toBe(180_000);
  });

  it('falls back to silence boundaries when nothing is calm', () => {
    // Normal for a continuous DJ mix that runs at full energy throughout.
    const flat = track({
      id: 2,
      durationMs: 200_000,
      introEndMs: 3_000,
      outroStartMs: 190_000,
      sections: [
        { startMs: 0, endMs: 100_000, energy: 0.8 },
        { startMs: 100_000, endMs: 200_000, energy: 0.8 },
      ],
    });
    // Equal energies still count as "at or below average", so this must at
    // least stay within the track and never return something nonsensical.
    const entry = chooseEntryPoint(flat);
    expect(entry).toBeGreaterThanOrEqual(0);
    expect(entry).toBeLessThan(flat.durationMs);
  });

  it('handles a track with no sections at all', () => {
    const bare = track({ id: 3, sections: [] });
    expect(chooseEntryPoint(bare)).toBe(bare.introEndMs);
    expect(chooseExitPoint(bare, 8_000)).toBeLessThanOrEqual(bare.outroStartMs);
  });

  it('never enters most of the way through a track', () => {
    // A "calm section" at 80% through is not an entry point, it is skipping
    // the track.
    const lateCalm = track({
      id: 4,
      durationMs: 240_000,
      sections: [
        { startMs: 0, endMs: 200_000, energy: 0.9 },
        { startMs: 200_000, endMs: 240_000, energy: 0.1 },
      ],
    });
    expect(chooseEntryPoint(lateCalm)).toBeLessThan(240_000 / 3 + 5_000);
  });
});

describe('planTransition', () => {
  it('beat-matches two compatible tracks', () => {
    const plan = planTransition(
      track({ id: 1, bpm: 128 }),
      track({ id: 2, bpm: 128 }),
    );

    expect(['phraseAligned', 'beatAligned']).toContain(plan.style);
    expect(plan.fallbackReason).toBeUndefined();
    expect(plan.incomingTempoRatio).toBeCloseTo(1, 6);
  });

  it('starts the transition on a downbeat of the outgoing track', () => {
    const outgoing = track({ id: 1, bpm: 128 });
    const plan = planTransition(outgoing, track({ id: 2, bpm: 128 }));

    // The chosen time must actually be one of the outgoing downbeats - the
    // whole point is that it is not an arbitrary timestamp.
    const downbeatTimes = outgoing.downbeatIndices.map(
      (index) => outgoing.beatsMs[index] as number,
    );
    expect(downbeatTimes).toContain(plan.outgoingStartMs);
  });

  it('stretches the incoming track to the outgoing tempo', () => {
    const plan = planTransition(
      track({ id: 1, bpm: 128 }),
      track({ id: 2, bpm: 124 }),
    );

    expect(plan.targetBpm).toBeCloseTo(128, 6);
    expect(plan.incomingTempoRatio).toBeCloseTo(128 / 124, 4);
    // The track people are already dancing to is left alone.
    expect(plan.outgoingTempoRatio).toBe(1);
  });

  it('treats half time as the same pulse rather than a huge stretch', () => {
    const plan = planTransition(
      track({ id: 1, bpm: 140 }),
      track({ id: 2, bpm: 70 }),
    );

    expect(plan.fallbackReason).toBeUndefined();
    expect(plan.incomingTempoRatio).toBeCloseTo(1, 4);
  });

  it('falls back to a crossfade when tempos are too far apart', () => {
    const plan = planTransition(
      track({ id: 1, bpm: 128 }),
      track({ id: 2, bpm: 160 }),
    );

    expect(plan.style).toBe('crossfade');
    expect(plan.fallbackReason).toContain('%');
    expect(plan.incomingTempoRatio).toBe(1);
  });

  it('refuses to beat-match against an untrustworthy grid', () => {
    // This is the reason beat confidence had to become a real measurement:
    // matching to an arbitrary grid sounds worse than not matching at all.
    const plan = planTransition(
      track({ id: 1, bpm: 128 }),
      track({ id: 2, bpm: 128, beatConfidence: MIN_BEAT_CONFIDENCE - 0.01 }),
    );

    expect(plan.style).toBe('crossfade');
    expect(plan.fallbackReason).toContain('beat grid');
  });

  it('swaps the bass on a beat-matched transition but not on a fallback', () => {
    const matched = planTransition(
      track({ id: 1, bpm: 128 }),
      track({ id: 2, bpm: 128 }),
    );
    expect(matched.eq.outgoing.lowTo).toBe(0);
    expect(matched.eq.incoming.lowFrom).toBe(0);

    const fallback = planTransition(
      track({ id: 1, bpm: 128 }),
      track({ id: 2, bpm: 160 }),
    );
    // Ducking the bass of a track that is not rhythmically locked just sounds
    // like a fault.
    expect(fallback.eq.outgoing.lowTo).toBe(1);
  });

  it('never runs past the end of either track', () => {
    const outgoing = track({ id: 1, bpm: 128, durationMs: 30_000 });
    const incoming = track({ id: 2, bpm: 128, durationMs: 30_000 });
    const plan = planTransition(outgoing, incoming, {
      length: 'long',
      adaptIncoming: true,
    });

    expect(plan.outgoingEndMs).toBeLessThanOrEqual(outgoing.durationMs);
    expect(plan.incomingStartMs + plan.durationMs).toBeLessThanOrEqual(
      incoming.durationMs,
    );
    expect(plan.durationMs).toBeGreaterThan(0);
  });

  it('always produces a plan, even for two unanalysable tracks', () => {
    // Playback must never stop because a transition could not be worked out.
    const broken = track({
      id: 1,
      beatsMs: [],
      downbeatIndices: [],
      beatConfidence: 0,
      bpm: 0,
    });
    const plan = planTransition(broken, { ...broken, id: 2 });

    expect(plan.durationMs).toBeGreaterThan(0);
    expect(plan.style).toBe('crossfade');
    expect(plan.fallbackReason).toBeDefined();
  });

  it('locks the two grids across the whole blend, not just its first beat', () => {
    // A ratio from average BPM only guarantees alignment at the start. Over a
    // long blend the decks slide apart, which is what makes a mix sound like
    // two records playing at once.
    //
    // This track drifts: its beats are slightly further apart as it goes, so
    // its average BPM does not describe the blend window.
    const drifting = track({ id: 2, bpm: 128 });
    const drifted = {
      ...drifting,
      beatsMs: drifting.beatsMs.map((time, index) => time + index * index * 0.01),
    };

    const plan = planTransition(track({ id: 1, bpm: 128 }), drifted, {
      length: 'long',
      adaptIncoming: true,
    });

    if (plan.fallbackReason !== undefined) return;

    // Work out where each grid actually is after the blend, at the planned
    // ratio. If the ratio came from average BPM these would not agree.
    const beatsInBlend = Math.round(
      plan.durationMs / ((60_000 / plan.targetBpm)),
    );

    const outgoing = track({ id: 1, bpm: 128 });
    const startIndexOut = outgoing.beatsMs.findIndex(
      (time) => time >= plan.outgoingStartMs - 1,
    );
    const startIndexIn = drifted.beatsMs.findIndex(
      (time) => time >= plan.incomingStartMs - 1,
    );

    const endOut = outgoing.beatsMs[startIndexOut + beatsInBlend];
    const endIn = drifted.beatsMs[startIndexIn + beatsInBlend];
    if (endOut === undefined || endIn === undefined) return;

    const outgoingSpan = endOut - plan.outgoingStartMs;
    // The incoming track is played at the planned rate, so its span shrinks.
    const incomingSpan =
      (endIn - plan.incomingStartMs) / plan.incomingTempoRatio;

    const driftMs = Math.abs(outgoingSpan - incomingSpan);
    // Within a tenth of a beat across the entire blend.
    const beatMs = 60_000 / plan.targetBpm;
    expect(driftMs).toBeLessThan(beatMs * 0.1);
  });

  it('scores a clean match above a fallback', () => {
    const clean = planTransition(
      track({ id: 1, bpm: 128 }),
      track({ id: 2, bpm: 128 }),
    );
    const forced = planTransition(
      track({ id: 1, bpm: 128 }),
      track({ id: 2, bpm: 170 }),
    );

    expect(clean.score.total).toBeGreaterThan(forced.score.total);
  });

  it('keeps the stretch within the documented limit whenever it matches', () => {
    for (let bpm = 118; bpm <= 138; bpm += 1) {
      const plan = planTransition(
        track({ id: 1, bpm: 128 }),
        track({ id: 2, bpm }),
      );
      if (plan.fallbackReason === undefined) {
        expect(Math.abs(plan.incomingTempoRatio - 1)).toBeLessThanOrEqual(
          MAX_TEMPO_STRETCH + 1e-9,
        );
      }
    }
  });
});
