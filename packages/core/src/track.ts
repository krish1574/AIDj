/**
 * Track identity and analysis contracts.
 *
 * NOTE ON SCOPE: the analysis fields below are the contract that Milestone 3
 * will populate. Nothing in Milestone 1 produces them. They live here now
 * because the Convex schema and the SQLite cache both need to agree on the
 * shape before either is written, and because `ANALYSIS_VERSION` is the cache
 * invalidation key for the entire pipeline.
 */

/**
 * Bump whenever any analysis algorithm changes in a way that makes previously
 * cached results wrong. Cached analysis with a different version is discarded
 * and the track is re-analysed. Never reuse an old number.
 */
export const ANALYSIS_VERSION = 3;
/*
 * 3 - beat confidence is contrast x hit rate. Version 2 multiplied contrast by
 *     the uniformity of on-beat strengths, which punished musical dynamics:
 *     measured on real tracks it scored a mastered DJ edit at 0.05 and a
 *     monotonous voice memo at 0.40, i.e. exactly backwards.
 * 2 - beat confidence measured on-beat vs between-beat contrast. Version 1
 *     compared on-beat strength against the overall mean, which saturated at
 *     1.0 for every track including speech, so the planner had no way to
 *     distinguish a solid grid from a meaningless one.
 */

export interface TrackIdentity {
  /**
   * Stable content identity. Derived from a hash of the first and last 1 MiB
   * of the file plus its byte length - full-file hashing is too slow for a
   * multi-thousand-track library scan, and this is collision-safe enough for
   * a personal library. Survives file moves and renames, which a URI does not.
   */
  contentHash: string;
  /** Platform URI. May become invalid; never used as identity. */
  uri: string;
  sizeBytes: number;
  modifiedAtMs: number;
}

export interface TrackMetadata {
  title: string;
  artist: string | null;
  album: string | null;
  durationMs: number;
  /** Container/codec as reported by the extractor, e.g. "audio/mpeg". */
  mimeType: string;
  sampleRate: number;
  channelCount: number;
}

export type MusicalMode = 'major' | 'minor';

export interface MusicalKey {
  /** 0 = C, 1 = C#, ... 11 = B. */
  tonic: number;
  mode: MusicalMode;
}

export interface Section {
  startMs: number;
  endMs: number;
  /** Normalised 0..1 short-term loudness within this section. */
  energy: number;
  /** Strength of the boundary that opened this section, 0..1. */
  novelty: number;
}

/**
 * Confidence values are all 0..1 and are load-bearing, not decorative: the
 * transition planner degrades to safer, longer, non-beat-locked transitions
 * when confidence is low rather than acting on a grid it does not trust.
 */
export interface TrackAnalysis {
  analysisVersion: number;
  bpm: number;
  bpmConfidence: number;
  /** Offset of the first detected beat, milliseconds from file start. */
  firstBeatMs: number;
  /**
   * Number of beats in the grid. The grid itself is a non-uniform float32
   * array of beat timestamps stored as a separate binary blob (thousands of
   * values - wrong shape for a document store), addressed by contentHash.
   * `bpm` is the grid's average, not a substitute for it.
   */
  beatCount: number;
  /** Indices into the beat grid that are downbeats (start of a bar). */
  downbeatIndices: readonly number[];
  key: MusicalKey;
  keyConfidence: number;
  /** ITU-R BS.1770-4 integrated loudness, LUFS. Negative. */
  integratedLufs: number;
  /** True-peak, dBTP. Negative for headroom. */
  truePeakDbtp: number;
  /** Whole-track energy 0..1. */
  energy: number;
  sections: readonly Section[];
  introEndMs: number;
  outroStartMs: number;
  /**
   * Heuristic vocal-presence probability, 0..1, sampled on a fixed grid.
   * This is NOT stem separation and is not reliable enough to gate a
   * transition on its own - see docs/known-limitations.md.
   */
  vocalActivity: readonly number[];
  vocalActivityHopMs: number;
}

export interface TrackProfile {
  identity: TrackIdentity;
  metadata: TrackMetadata;
  analysis: TrackAnalysis | null;
}
