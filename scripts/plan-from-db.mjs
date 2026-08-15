#!/usr/bin/env node
/**
 * Runs the queue planner over analysis pulled from a device database.
 *
 * The planner's unit tests use fixtures with clean, deliberately chosen
 * numbers. This exercises it against real analysis - low confidences, odd
 * tempi, duplicate tracks and all - which is where scoring weights are
 * actually judged. It is a development tool, not part of the app.
 *
 * Usage:
 *   node scripts/plan-from-db.mjs <path-to-aidj.db> [smooth|balanced|energetic]
 */
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { MIX_STYLES, explainChoice, planQueue } from '../packages/core/src/planner.ts';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const databasePath = process.argv[2];
const styleName = process.argv[3] ?? 'balanced';

if (databasePath === undefined || !existsSync(databasePath)) {
  console.error('Usage: node scripts/plan-from-db.mjs <aidj.db> [style]');
  console.error('Pull one with: adb exec-out run-as dj.ai.app cat files/SQLite/aidj.db > aidj.db');
  process.exit(1);
}

const style = MIX_STYLES[styleName];
if (style === undefined) {
  console.error(`Unknown style "${styleName}". Use smooth, balanced or energetic.`);
  process.exit(1);
}

const sqlite =
  process.env.SQLITE ??
  join(
    process.env.ANDROID_HOME ?? 'C:/Users/Krish/dev-tools/android-sdk',
    'platform-tools',
    'sqlite3.exe',
  );

/**
 * Only analysed tracks can be planned, and only the current analysis version
 * counts - stale rows would silently plan against superseded numbers.
 */
const query = `
  SELECT t.id, t.title, t.duration_ms, a.bpm, a.bpm_confidence, a.alternate_bpm,
         a.key_tonic, a.key_mode, a.key_confidence, a.energy, a.integrated_lufs
  FROM track_analysis a
  JOIN tracks t ON t.id = a.track_id
  WHERE a.analysis_version = (SELECT MAX(analysis_version) FROM track_analysis)
  ORDER BY t.title;
`;

const result = spawnSync(sqlite, ['-json', databasePath, query], {
  encoding: 'utf8',
});

if (result.status !== 0) {
  console.error(result.stderr || 'sqlite3 failed');
  process.exit(1);
}

const rows = JSON.parse(result.stdout || '[]');
if (rows.length === 0) {
  console.error('No analysed tracks found in that database.');
  process.exit(1);
}

const tracks = rows.map((row, index) => ({
  id: row.id,
  title: row.title,
  originalIndex: index,
  durationMs: row.duration_ms,
  bpm: row.bpm,
  bpmConfidence: row.bpm_confidence,
  alternateBpm: row.alternate_bpm,
  key: { tonic: row.key_tonic, mode: row.key_mode === 0 ? 'major' : 'minor' },
  keyConfidence: row.key_confidence,
  energy: row.energy,
  integratedLufs: row.integrated_lufs,
  // Not yet stored per track; the planner weights it low and this tool is for
  // judging tempo/key/energy behaviour.
  vocalActivity: 0.5,
}));

const queue = planQueue(tracks, style);

const moved = queue.filter(
  (item, position) => item.track.originalIndex !== position,
).length;

console.log(`\nStyle: ${styleName}   Tracks: ${tracks.length}   Moved: ${moved}\n`);

for (const [position, item] of queue.entries()) {
  const displaced = item.track.originalIndex - position;
  const marker = displaced === 0 ? ' ' : displaced > 0 ? '^' : 'v';
  const bpm = item.track.bpm.toFixed(1).padStart(5);
  const title = item.track.title.slice(0, 34).padEnd(34);
  const reason = explainChoice(item);
  console.log(
    `${String(position + 1).padStart(2)}. ${marker} ${bpm}  ${title}  ${reason}`,
  );
}

console.log('');
