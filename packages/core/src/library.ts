/**
 * Local music library and playlist contracts.
 *
 * These are deliberately separate from `TrackProfile` in track.ts. A library
 * row is what a *scan* can know - filename, tags, duration - and is available
 * within seconds of opening the app. A TrackProfile additionally carries
 * analysis, which costs seconds of DSP per track and only exists after
 * Milestone 3. Conflating them would force the library UI to wait on analysis.
 */

/**
 * A track as discovered on the device.
 *
 * `contentHash` is nullable here, unlike in `TrackIdentity`. Hashing reads 2 MiB
 * per file, which is far too slow to do for every row during a scan of a
 * multi-thousand-track library. It is computed lazily - when a track is first
 * added to a playlist - because that is the first moment identity has to be
 * stable across devices and file moves.
 */
export interface LibraryTrack {
  /** Local row id, assigned by SQLite. Never synced. */
  id: number;
  /**
   * MediaStore's row id. Stable while the file is not re-indexed, and cheap to
   * query, so it is the scan-time dedupe key. Not durable identity - a factory
   * reset or SD card remount changes it, which is why contentHash exists.
   */
  mediaStoreId: number;
  contentUri: string;
  contentHash: string | null;
  title: string;
  artist: string | null;
  album: string | null;
  durationMs: number;
  mimeType: string;
  sizeBytes: number;
  modifiedAtMs: number;
  /** content:// URI for album art, when MediaStore has one. */
  artworkUri: string | null;
  /** Epoch ms this row was last confirmed present on disk. */
  lastSeenAtMs: number;
}

/** What the native scanner returns, before any local id exists. */
export type ScannedTrack = Omit<
  LibraryTrack,
  'id' | 'contentHash' | 'lastSeenAtMs'
>;

export interface Playlist {
  id: number;
  name: string;
  createdAtMs: number;
  updatedAtMs: number;
  trackCount: number;
}

export interface PlaylistEntry {
  playlistId: number;
  trackId: number;
  /**
   * Sparse ordering key. Gaps are intentional: reordering one item rewrites
   * one row instead of renumbering the whole playlist, which matters for the
   * drag-to-reorder UI and for eventual conflict-free sync.
   */
  position: number;
}

/** Spacing between adjacent positions when a playlist is built or compacted. */
export const POSITION_STEP = 1024;

/**
 * Position for an item dropped between `before` and `after`.
 *
 * Returns null when the gap is exhausted, which is the caller's signal to
 * compact the playlist. Repeated reordering into the same gap halves it each
 * time, so with POSITION_STEP of 1024 this happens after ~10 moves into one
 * spot - rare, but it must be handled rather than silently colliding.
 */
export function positionBetween(
  before: number | null,
  after: number | null,
): number | null {
  if (before === null && after === null) return POSITION_STEP;
  if (before === null) return (after as number) - POSITION_STEP;
  if (after === null) return before + POSITION_STEP;

  const gap = after - before;
  if (gap <= 1) return null;
  return before + Math.floor(gap / 2);
}

/** Evenly spaced positions, used after a compaction. */
export function compactPositions(count: number): number[] {
  return Array.from({ length: count }, (_, index) => (index + 1) * POSITION_STEP);
}

/** Display helper: "3:07". Durations are milliseconds everywhere internally. */
export function formatDuration(durationMs: number): string {
  if (!Number.isFinite(durationMs) || durationMs < 0) return '0:00';
  const totalSeconds = Math.floor(durationMs / 1000);
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;
  return `${minutes}:${seconds.toString().padStart(2, '0')}`;
}
