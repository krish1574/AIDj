/** User-facing DJ preferences. Shared by the app, the API and Convex. */

export type MixStyle = 'smooth' | 'balanced' | 'energetic';
export type TransitionLength = 'short' | 'medium' | 'long';

export interface UserPreferences {
  mixStyle: MixStyle;
  transitionLength: TransitionLength;
  /**
   * When false the queue planner emits the playlist in its original order and
   * only chooses transition points. Respecting playlist intent is the default
   * posture; reordering is opt-in improvement, not a licence to shuffle.
   */
  allowReordering: boolean;
  /** Normalisation target, LUFS. -14 matches common streaming practice. */
  targetLufs: number;
}

export const DEFAULT_PREFERENCES: UserPreferences = {
  mixStyle: 'balanced',
  transitionLength: 'medium',
  allowReordering: true,
  targetLufs: -14,
};

/**
 * Nominal transition durations in milliseconds. The transition planner
 * (Milestone 5) treats these as a target and snaps to the nearest musical
 * phrase boundary; it does not use them literally.
 */
export const TRANSITION_LENGTH_MS: Record<TransitionLength, number> = {
  short: 4_000,
  medium: 8_000,
  long: 16_000,
};
