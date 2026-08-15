import { Directory, File, Paths } from 'expo-file-system';

import { ANALYSIS_VERSION, type LibraryTrack } from '@ai-dj/core';
import type { NativeAnalysisResult } from 'aidj-analysis';

import { openDatabase } from './schema';

/**
 * Analysis cache.
 *
 * Analysis costs seconds to minutes per track, so it is computed once and
 * invalidated only when the file changes or the algorithms do. The cache key
 * is (track, ANALYSIS_VERSION, size, modified time): a file edited in place
 * keeps its MediaStore id, so identity alone is not enough.
 */

export interface StoredAnalysis {
  trackId: number;
  analysisVersion: number;
  durationMs: number;
  bpm: number;
  bpmConfidence: number;
  alternateBpm: number;
  beatConfidence: number;
  downbeatConfidence: number;
  beatsPerBar: number;
  beatCount: number;
  keyTonic: number;
  keyMode: number;
  keyConfidence: number;
  integratedLufs: number;
  peakDbfs: number;
  energy: number;
  introEndMs: number;
  outroStartMs: number;
  analysisSampleRate: number;
  beatGridPath: string | null;
  sections: { startMs: number; endMs: number; energy: number; novelty: number }[];
  analysedAtMs: number;
}

function beatGridDirectory(): Directory {
  const directory = new Directory(Paths.document, 'analysis');
  if (!directory.exists) directory.create({ intermediates: true });
  return directory;
}

/**
 * Writes the beat grid as raw little-endian float64.
 *
 * Binary rather than JSON: a 50 minute mix has ~10,000 beats, which is ~180 KB
 * of JSON versus 80 KB binary, and parsing it back costs real time on every
 * session start.
 */
async function writeBeatGrid(
  trackId: number,
  beatsMs: readonly number[],
): Promise<string | null> {
  if (beatsMs.length === 0) return null;

  const buffer = new ArrayBuffer(beatsMs.length * 8);
  const view = new DataView(buffer);
  for (let i = 0; i < beatsMs.length; i += 1) {
    view.setFloat64(i * 8, beatsMs[i] as number, true);
  }

  const file = new File(beatGridDirectory(), `beats-${trackId}.bin`);
  if (file.exists) file.delete();
  file.create();
  file.write(new Uint8Array(buffer));
  return file.uri;
}

export async function readBeatGrid(path: string): Promise<Float64Array> {
  const file = new File(path);
  if (!file.exists) return new Float64Array(0);
  const bytes = await file.bytes();
  return new Float64Array(
    bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
  );
}

export async function saveAnalysis(
  track: LibraryTrack,
  result: NativeAnalysisResult,
): Promise<void> {
  const db = await openDatabase();
  const beatGridPath = await writeBeatGrid(track.id, result.beatsMs);

  await db.runAsync(
    `INSERT OR REPLACE INTO track_analysis (
       track_id, analysis_version, size_bytes, modified_at_ms,
       duration_ms, bpm, bpm_confidence, alternate_bpm,
       beat_confidence, downbeat_confidence, beats_per_bar, beat_count,
       key_tonic, key_mode, key_confidence,
       integrated_lufs, peak_dbfs, energy,
       intro_end_ms, outro_start_ms, analysis_sample_rate,
       beat_grid_path, sections_json, analysed_at_ms
     ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);`,
    [
      track.id,
      ANALYSIS_VERSION,
      track.sizeBytes,
      track.modifiedAtMs,
      result.durationMs,
      result.bpm,
      result.bpmConfidence,
      result.alternateBpm,
      result.beatConfidence,
      result.downbeatConfidence,
      result.beatsPerBar,
      result.beatCount,
      result.keyTonic,
      result.keyMode,
      result.keyConfidence,
      result.integratedLufs,
      result.peakDbfs,
      result.energy,
      result.introEndMs,
      result.outroStartMs,
      result.analysisSampleRate,
      beatGridPath,
      JSON.stringify(result.sections),
      Date.now(),
    ],
  );

  // A track that now analyses is no longer a failure.
  await db.runAsync('DELETE FROM analysis_failures WHERE track_id = ?;', [
    track.id,
  ]);

  // Drop rows left by older analysis versions. Every read already filters on
  // ANALYSIS_VERSION so stale rows are never used, but without this they
  // accumulate a full copy of the library per version bump - and during
  // development that happens often.
  await db.runAsync('DELETE FROM track_analysis WHERE analysis_version != ?;', [
    ANALYSIS_VERSION,
  ]);
}

export async function recordFailure(
  trackId: number,
  reason: string,
): Promise<void> {
  const db = await openDatabase();
  await db.runAsync(
    `INSERT INTO analysis_failures (track_id, reason, attempts, last_attempt_ms)
     VALUES (?, ?, 1, ?)
     ON CONFLICT (track_id) DO UPDATE SET
       reason = excluded.reason,
       attempts = attempts + 1,
       last_attempt_ms = excluded.last_attempt_ms;`,
    [trackId, reason, Date.now()],
  );
}

/**
 * Tracks still needing analysis, longest-shortest excluded.
 *
 * Ordered by duration ascending so a full-library pass produces many finished
 * tracks early rather than spending its first several minutes on one 57 minute
 * mix. Repeatedly failing tracks are skipped after three attempts - retrying a
 * corrupt file on every pass wastes the budget that working tracks need.
 */
export async function tracksNeedingAnalysis(
  limit = 50,
): Promise<LibraryTrack[]> {
  const db = await openDatabase();
  const rows = await db.getAllAsync<Record<string, unknown>>(
    `SELECT t.* FROM tracks t
     LEFT JOIN track_analysis a
       ON a.track_id = t.id
      AND a.analysis_version = ?
      AND a.size_bytes = t.size_bytes
      AND a.modified_at_ms = t.modified_at_ms
     LEFT JOIN analysis_failures f ON f.track_id = t.id
     WHERE a.track_id IS NULL
       AND (f.attempts IS NULL OR f.attempts < 3)
     ORDER BY t.duration_ms ASC
     LIMIT ?;`,
    [ANALYSIS_VERSION, limit],
  );

  return rows.map((row) => ({
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

export async function getAnalysis(
  trackId: number,
): Promise<StoredAnalysis | null> {
  const db = await openDatabase();
  const row = await db.getFirstAsync<Record<string, unknown>>(
    `SELECT * FROM track_analysis WHERE track_id = ? AND analysis_version = ?;`,
    [trackId, ANALYSIS_VERSION],
  );
  if (row === null) return null;

  return {
    trackId: row.track_id as number,
    analysisVersion: row.analysis_version as number,
    durationMs: row.duration_ms as number,
    bpm: row.bpm as number,
    bpmConfidence: row.bpm_confidence as number,
    alternateBpm: row.alternate_bpm as number,
    beatConfidence: row.beat_confidence as number,
    downbeatConfidence: row.downbeat_confidence as number,
    beatsPerBar: row.beats_per_bar as number,
    beatCount: row.beat_count as number,
    keyTonic: row.key_tonic as number,
    keyMode: row.key_mode as number,
    keyConfidence: row.key_confidence as number,
    integratedLufs: row.integrated_lufs as number,
    peakDbfs: row.peak_dbfs as number,
    energy: row.energy as number,
    introEndMs: row.intro_end_ms as number,
    outroStartMs: row.outro_start_ms as number,
    analysisSampleRate: row.analysis_sample_rate as number,
    beatGridPath: (row.beat_grid_path as string | null) ?? null,
    sections: JSON.parse(row.sections_json as string),
    analysedAtMs: row.analysed_at_ms as number,
  };
}

export interface AnalysisCoverage {
  total: number;
  analysed: number;
  failed: number;
}

export async function analysisCoverage(): Promise<AnalysisCoverage> {
  const db = await openDatabase();
  const row = await db.getFirstAsync<AnalysisCoverage>(
    `SELECT
       (SELECT COUNT(*) FROM tracks) AS total,
       (SELECT COUNT(*) FROM track_analysis a JOIN tracks t ON t.id = a.track_id
         WHERE a.analysis_version = ?
           AND a.size_bytes = t.size_bytes
           AND a.modified_at_ms = t.modified_at_ms) AS analysed,
       (SELECT COUNT(*) FROM analysis_failures WHERE attempts >= 3) AS failed;`,
    [ANALYSIS_VERSION],
  );
  return row ?? { total: 0, analysed: 0, failed: 0 };
}
