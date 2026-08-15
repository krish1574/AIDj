import { describe, it, expect } from 'vitest';

import { DEFAULT_PREFERENCES, TRANSITION_LENGTH_MS } from './preferences';
import { ANALYSIS_VERSION } from './track';
import { ENGINE_SAMPLE_RATE, VOICE_COUNT } from './engine';
import { isApiSuccess } from './api';
import type { ApiResponse } from './api';

/**
 * These are contract tests, not logic tests. The values below are duplicated
 * in C++ (`AudioTypes.h`) and in the Convex schema, and a silent divergence
 * would produce wrong audio or a rejected write rather than a compile error.
 */
describe('engine constants', () => {
  it('matches the fixed internal format the C++ mixer assumes', () => {
    expect(ENGINE_SAMPLE_RATE).toBe(48_000);
    expect(VOICE_COUNT).toBe(2);
  });
});

describe('analysis versioning', () => {
  it('is a positive integer, since it is the cache invalidation key', () => {
    expect(Number.isInteger(ANALYSIS_VERSION)).toBe(true);
    expect(ANALYSIS_VERSION).toBeGreaterThan(0);
  });
});

describe('default preferences', () => {
  it('respects playlist intent by default rather than shuffling', () => {
    expect(DEFAULT_PREFERENCES.mixStyle).toBe('balanced');
    expect(DEFAULT_PREFERENCES.transitionLength).toBe('medium');
  });

  it('targets a loudness inside the guard rails the backend enforces', () => {
    expect(DEFAULT_PREFERENCES.targetLufs).toBeLessThanOrEqual(-8);
    expect(DEFAULT_PREFERENCES.targetLufs).toBeGreaterThanOrEqual(-24);
  });

  it('orders transition lengths monotonically', () => {
    expect(TRANSITION_LENGTH_MS.short).toBeLessThan(TRANSITION_LENGTH_MS.medium);
    expect(TRANSITION_LENGTH_MS.medium).toBeLessThan(TRANSITION_LENGTH_MS.long);
  });
});

describe('api envelope', () => {
  it('narrows a success response', () => {
    const response: ApiResponse<number> = {
      success: true,
      data: 42,
      message: 'Success',
    };
    expect(isApiSuccess(response)).toBe(true);
    if (isApiSuccess(response)) expect(response.data).toBe(42);
  });

  it('narrows a failure response', () => {
    const response: ApiResponse<number> = {
      success: false,
      error: { code: 'NOT_FOUND', message: 'Nope.' },
    };
    expect(isApiSuccess(response)).toBe(false);
  });
});
