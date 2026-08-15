import { describe, expect, it } from 'vitest';

import {
  POSITION_STEP,
  compactPositions,
  formatDuration,
  positionBetween,
} from './library';

describe('positionBetween', () => {
  it('seeds the first position', () => {
    expect(positionBetween(null, null)).toBe(POSITION_STEP);
  });

  it('appends after the last item', () => {
    expect(positionBetween(5000, null)).toBe(5000 + POSITION_STEP);
  });

  it('prepends before the first item', () => {
    expect(positionBetween(null, 5000)).toBe(5000 - POSITION_STEP);
  });

  it('lands strictly between two neighbours', () => {
    const position = positionBetween(1024, 2048);
    expect(position).not.toBeNull();
    expect(position as number).toBeGreaterThan(1024);
    expect(position as number).toBeLessThan(2048);
  });

  it('reports exhaustion instead of colliding', () => {
    // Adjacent integers leave no room for a new key; the caller must compact.
    expect(positionBetween(10, 11)).toBeNull();
    expect(positionBetween(10, 10)).toBeNull();
  });

  it('survives repeated insertion into the same gap until exhausted', () => {
    let before = 0;
    const after = POSITION_STEP;
    // 1024 -> 512 -> 256 ... halves ten times before the neighbours become
    // adjacent integers and no key fits between them.
    for (let i = 0; i < 10; i += 1) {
      const next = positionBetween(before, after);
      expect(next).not.toBeNull();
      before = next as number;
    }
    expect(before).toBe(after - 1);
    expect(positionBetween(before, after)).toBeNull();
  });
});

describe('compactPositions', () => {
  it('produces evenly spaced ascending keys', () => {
    expect(compactPositions(3)).toEqual([1024, 2048, 3072]);
  });

  it('handles an empty playlist', () => {
    expect(compactPositions(0)).toEqual([]);
  });
});

describe('formatDuration', () => {
  it('formats minutes and zero-padded seconds', () => {
    expect(formatDuration(187_000)).toBe('3:07');
    expect(formatDuration(60_000)).toBe('1:00');
    expect(formatDuration(0)).toBe('0:00');
  });

  it('does not render NaN or negative durations', () => {
    expect(formatDuration(Number.NaN)).toBe('0:00');
    expect(formatDuration(-1)).toBe('0:00');
  });
});
