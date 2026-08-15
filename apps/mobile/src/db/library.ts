import type { LibraryTrack, ScannedTrack } from '@ai-dj/core';

import { openDatabase } from './schema';

interface TrackRow {
  id: number;
  media_store_id: number;
  content_uri: string;
  content_hash: string | null;
  title: string;
  artist: string | null;
  album: string | null;
  duration_ms: number;
  mime_type: string;
  size_bytes: number;
  modified_at_ms: number;
  artwork_uri: string | null;
  last_seen_at_ms: number;
}

function toTrack(row: TrackRow): LibraryTrack {
  return {
    id: row.id,
    mediaStoreId: row.media_store_id,
    contentUri: row.content_uri,
    contentHash: row.content_hash,
    title: row.title,
    artist: row.artist,
    album: row.album,
    durationMs: row.duration_ms,
    mimeType: row.mime_type,
    sizeBytes: row.size_bytes,
    modifiedAtMs: row.modified_at_ms,
    artworkUri: row.artwork_uri,
    lastSeenAtMs: row.last_seen_at_ms,
  };
}

export interface ScanResult {
  added: number;
  updated: number;
  removed: number;
  total: number;
}

/**
 * Reconciles a scan against the stored library.
 *
 * Upsert rather than replace, for two reasons: track ids are referenced by
 * playlists and must survive a rescan, and `content_hash` is expensive to
 * compute so it must not be discarded when a row is merely re-seen.
 *
 * Rows absent from the scan are deleted. That cascades to playlist entries,
 * which is correct - a track whose file is gone cannot be played - but it does
 * mean an unmounted SD card empties playlists. Guarded against below by
 * refusing to treat an empty scan as "everything was deleted".
 */
export async function reconcileScan(
  scanned: readonly ScannedTrack[],
): Promise<ScanResult> {
  const db = await openDatabase();
  const now = Date.now();

  const existingCount = await countTracks();
  if (scanned.length === 0 && existingCount > 0) {
    // An empty result from a device that previously had music almost always
    // means revoked permission or an unmounted volume, not a deleted library.
    return { added: 0, updated: 0, removed: 0, total: existingCount };
  }

  let added = 0;
  let updated = 0;

  await db.withTransactionAsync(async () => {
    for (const track of scanned) {
      const existing = await db.getFirstAsync<{ id: number }>(
        'SELECT id FROM tracks WHERE media_store_id = ?;',
        [track.mediaStoreId],
      );

      if (existing) {
        await db.runAsync(
          `UPDATE tracks SET
             content_uri = ?, title = ?, artist = ?, album = ?,
             duration_ms = ?, mime_type = ?, size_bytes = ?,
             modified_at_ms = ?, artwork_uri = ?, last_seen_at_ms = ?,
             content_hash = CASE
               WHEN modified_at_ms != ? OR size_bytes != ? THEN NULL
               ELSE content_hash
             END
           WHERE id = ?;`,
          [
            track.contentUri,
            track.title,
            track.artist,
            track.album,
            track.durationMs,
            track.mimeType,
            track.sizeBytes,
            track.modifiedAtMs,
            track.artworkUri,
            now,
            // The file changed underneath us, so any cached identity is stale.
            track.modifiedAtMs,
            track.sizeBytes,
            existing.id,
          ],
        );
        updated += 1;
      } else {
        await db.runAsync(
          `INSERT INTO tracks (
             media_store_id, content_uri, content_hash, title, artist, album,
             duration_ms, mime_type, size_bytes, modified_at_ms, artwork_uri,
             last_seen_at_ms
           ) VALUES (?, ?, NULL, ?, ?, ?, ?, ?, ?, ?, ?, ?);`,
          [
            track.mediaStoreId,
            track.contentUri,
            track.title,
            track.artist,
            track.album,
            track.durationMs,
            track.mimeType,
            track.sizeBytes,
            track.modifiedAtMs,
            track.artworkUri,
            now,
          ],
        );
        added += 1;
      }
    }
  });

  const removedResult = await db.runAsync(
    'DELETE FROM tracks WHERE last_seen_at_ms < ?;',
    [now],
  );

  return {
    added,
    updated,
    removed: removedResult.changes,
    total: await countTracks(),
  };
}

export async function countTracks(): Promise<number> {
  const db = await openDatabase();
  const row = await db.getFirstAsync<{ count: number }>(
    'SELECT COUNT(*) AS count FROM tracks;',
  );
  return row?.count ?? 0;
}

/**
 * Library listing. `query` matches title, artist or album.
 * Paged because a large library must not be materialised into JS at once.
 */
export async function listTracks(options: {
  query?: string;
  limit?: number;
  offset?: number;
} = {}): Promise<LibraryTrack[]> {
  const db = await openDatabase();
  const { query, limit = 200, offset = 0 } = options;

  if (query && query.trim().length > 0) {
    const pattern = `%${query.trim()}%`;
    const rows = await db.getAllAsync<TrackRow>(
      `SELECT * FROM tracks
       WHERE title LIKE ? COLLATE NOCASE
          OR artist LIKE ? COLLATE NOCASE
          OR album LIKE ? COLLATE NOCASE
       ORDER BY title COLLATE NOCASE
       LIMIT ? OFFSET ?;`,
      [pattern, pattern, pattern, limit, offset],
    );
    return rows.map(toTrack);
  }

  const rows = await db.getAllAsync<TrackRow>(
    `SELECT * FROM tracks
     ORDER BY title COLLATE NOCASE
     LIMIT ? OFFSET ?;`,
    [limit, offset],
  );
  return rows.map(toTrack);
}

export async function getTrack(id: number): Promise<LibraryTrack | null> {
  const db = await openDatabase();
  const row = await db.getFirstAsync<TrackRow>(
    'SELECT * FROM tracks WHERE id = ?;',
    [id],
  );
  return row ? toTrack(row) : null;
}

/** Stores a lazily computed content hash. See ScannedTrack for why it is lazy. */
export async function setContentHash(
  trackId: number,
  contentHash: string,
): Promise<void> {
  const db = await openDatabase();
  await db.runAsync('UPDATE tracks SET content_hash = ? WHERE id = ?;', [
    contentHash,
    trackId,
  ]);
}

export async function tracksMissingHash(
  ids: readonly number[],
): Promise<LibraryTrack[]> {
  if (ids.length === 0) return [];
  const db = await openDatabase();
  const placeholders = ids.map(() => '?').join(',');
  const rows = await db.getAllAsync<TrackRow>(
    `SELECT * FROM tracks WHERE content_hash IS NULL AND id IN (${placeholders});`,
    [...ids],
  );
  return rows.map(toTrack);
}
