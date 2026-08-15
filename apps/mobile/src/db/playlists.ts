import type { LibraryTrack, Playlist } from '@ai-dj/core';
import { POSITION_STEP, compactPositions, positionBetween } from '@ai-dj/core';

import { openDatabase } from './schema';

interface PlaylistRow {
  id: number;
  name: string;
  created_at_ms: number;
  updated_at_ms: number;
  track_count: number;
}

function toPlaylist(row: PlaylistRow): Playlist {
  return {
    id: row.id,
    name: row.name,
    createdAtMs: row.created_at_ms,
    updatedAtMs: row.updated_at_ms,
    trackCount: row.track_count,
  };
}

export async function listPlaylists(): Promise<Playlist[]> {
  const db = await openDatabase();
  const rows = await db.getAllAsync<PlaylistRow>(
    `SELECT p.*, COUNT(pt.track_id) AS track_count
     FROM playlists p
     LEFT JOIN playlist_tracks pt ON pt.playlist_id = p.id
     GROUP BY p.id
     ORDER BY p.updated_at_ms DESC;`,
  );
  return rows.map(toPlaylist);
}

export async function getPlaylist(id: number): Promise<Playlist | null> {
  const db = await openDatabase();
  const row = await db.getFirstAsync<PlaylistRow>(
    `SELECT p.*, COUNT(pt.track_id) AS track_count
     FROM playlists p
     LEFT JOIN playlist_tracks pt ON pt.playlist_id = p.id
     WHERE p.id = ?
     GROUP BY p.id;`,
    [id],
  );
  return row ? toPlaylist(row) : null;
}

export async function createPlaylist(name: string): Promise<number> {
  const trimmed = name.trim();
  if (trimmed.length === 0) throw new Error('Playlist name cannot be empty.');

  const db = await openDatabase();
  const now = Date.now();
  const result = await db.runAsync(
    'INSERT INTO playlists (name, created_at_ms, updated_at_ms) VALUES (?, ?, ?);',
    [trimmed, now, now],
  );
  return result.lastInsertRowId;
}

export async function renamePlaylist(id: number, name: string): Promise<void> {
  const trimmed = name.trim();
  if (trimmed.length === 0) throw new Error('Playlist name cannot be empty.');

  const db = await openDatabase();
  await db.runAsync(
    'UPDATE playlists SET name = ?, updated_at_ms = ? WHERE id = ?;',
    [trimmed, Date.now(), id],
  );
}

export async function deletePlaylist(id: number): Promise<void> {
  const db = await openDatabase();
  // playlist_tracks rows go with it via ON DELETE CASCADE. The tracks
  // themselves are untouched - deleting a playlist must never remove music.
  await db.runAsync('DELETE FROM playlists WHERE id = ?;', [id]);
}

/** Tracks in playlist order. */
export async function playlistTracks(
  playlistId: number,
): Promise<LibraryTrack[]> {
  const db = await openDatabase();
  const rows = await db.getAllAsync<Record<string, never>>(
    `SELECT t.* FROM playlist_tracks pt
     JOIN tracks t ON t.id = pt.track_id
     WHERE pt.playlist_id = ?
     ORDER BY pt.position;`,
    [playlistId],
  );

  return (rows as unknown as Array<Record<string, unknown>>).map((row) => ({
    id: row.id as number,
    mediaStoreId: row.media_store_id as number,
    contentUri: row.content_uri as string,
    contentHash: (row.content_hash as string | null) ?? null,
    title: row.title as string,
    artist: (row.artist as string | null) ?? null,
    album: (row.album as string | null) ?? null,
    durationMs: row.duration_ms as number,
    mimeType: row.mime_type as string,
    sizeBytes: row.size_bytes as number,
    modifiedAtMs: row.modified_at_ms as number,
    artworkUri: (row.artwork_uri as string | null) ?? null,
    lastSeenAtMs: row.last_seen_at_ms as number,
  }));
}

async function touch(playlistId: number): Promise<void> {
  const db = await openDatabase();
  await db.runAsync('UPDATE playlists SET updated_at_ms = ? WHERE id = ?;', [
    Date.now(),
    playlistId,
  ]);
}

/**
 * Appends tracks, skipping any already present.
 * Returns how many were actually added.
 */
export async function addTracks(
  playlistId: number,
  trackIds: readonly number[],
): Promise<number> {
  if (trackIds.length === 0) return 0;

  const db = await openDatabase();
  const last = await db.getFirstAsync<{ position: number | null }>(
    'SELECT MAX(position) AS position FROM playlist_tracks WHERE playlist_id = ?;',
    [playlistId],
  );

  let position = last?.position ?? 0;
  let inserted = 0;

  await db.withTransactionAsync(async () => {
    for (const trackId of trackIds) {
      position += POSITION_STEP;
      const result = await db.runAsync(
        `INSERT OR IGNORE INTO playlist_tracks (playlist_id, track_id, position)
         VALUES (?, ?, ?);`,
        [playlistId, trackId, position],
      );
      if (result.changes > 0) inserted += 1;
    }
  });

  await touch(playlistId);
  return inserted;
}

export async function removeTrack(
  playlistId: number,
  trackId: number,
): Promise<void> {
  const db = await openDatabase();
  await db.runAsync(
    'DELETE FROM playlist_tracks WHERE playlist_id = ? AND track_id = ?;',
    [playlistId, trackId],
  );
  await touch(playlistId);
}

/**
 * Moves `trackId` to sit at `toIndex` in the current ordering.
 *
 * Writes one row in the common case by picking a position between the new
 * neighbours. When the gap between them is exhausted the whole playlist is
 * renumbered - correct but O(n), and rare by design. See positionBetween().
 */
export async function moveTrack(
  playlistId: number,
  trackId: number,
  toIndex: number,
): Promise<void> {
  const db = await openDatabase();

  const rows = await db.getAllAsync<{ track_id: number; position: number }>(
    `SELECT track_id, position FROM playlist_tracks
     WHERE playlist_id = ? ORDER BY position;`,
    [playlistId],
  );

  const without = rows.filter((row) => row.track_id !== trackId);
  const clamped = Math.max(0, Math.min(toIndex, without.length));

  const before = clamped > 0 ? (without[clamped - 1]?.position ?? null) : null;
  const after =
    clamped < without.length ? (without[clamped]?.position ?? null) : null;
  const position = positionBetween(before, after);

  if (position !== null) {
    await db.runAsync(
      `UPDATE playlist_tracks SET position = ?
       WHERE playlist_id = ? AND track_id = ?;`,
      [position, playlistId, trackId],
    );
  } else {
    const reordered = [
      ...without.slice(0, clamped).map((row) => row.track_id),
      trackId,
      ...without.slice(clamped).map((row) => row.track_id),
    ];
    const positions = compactPositions(reordered.length);

    await db.withTransactionAsync(async () => {
      for (let index = 0; index < reordered.length; index += 1) {
        const nextPosition = positions[index];
        const id = reordered[index];
        if (nextPosition === undefined || id === undefined) continue;
        await db.runAsync(
          `UPDATE playlist_tracks SET position = ?
           WHERE playlist_id = ? AND track_id = ?;`,
          [nextPosition, playlistId, id],
        );
      }
    });
  }

  await touch(playlistId);
}
