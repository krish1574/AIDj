import * as SQLite from 'expo-sqlite';

/**
 * Local database.
 *
 * SQLite is the source of truth for playback: the app must work with no
 * network and no account, so the library, playlists and (from Milestone 3)
 * the analysis cache all live here. Convex holds the account-level mirror.
 *
 * Migrations are forward-only and idempotent. `user_version` is the migration
 * counter; never renumber past migrations, only append.
 */

const DATABASE_NAME = 'aidj.db';

let handle: SQLite.SQLiteDatabase | null = null;

type Migration = (db: SQLite.SQLiteDatabase) => Promise<void>;

const migrations: Migration[] = [
  // 1 - library and playlists
  async (db) => {
    await db.execAsync(`
      CREATE TABLE tracks (
        id             INTEGER PRIMARY KEY AUTOINCREMENT,
        media_store_id INTEGER NOT NULL UNIQUE,
        content_uri    TEXT    NOT NULL,
        content_hash   TEXT,
        title          TEXT    NOT NULL,
        artist         TEXT,
        album          TEXT,
        duration_ms    INTEGER NOT NULL,
        mime_type      TEXT    NOT NULL,
        size_bytes     INTEGER NOT NULL,
        modified_at_ms INTEGER NOT NULL,
        artwork_uri    TEXT,
        last_seen_at_ms INTEGER NOT NULL
      );

      CREATE INDEX idx_tracks_title  ON tracks (title COLLATE NOCASE);
      CREATE INDEX idx_tracks_artist ON tracks (artist COLLATE NOCASE);
      CREATE INDEX idx_tracks_hash   ON tracks (content_hash);

      CREATE TABLE playlists (
        id            INTEGER PRIMARY KEY AUTOINCREMENT,
        name          TEXT    NOT NULL,
        created_at_ms INTEGER NOT NULL,
        updated_at_ms INTEGER NOT NULL
      );

      CREATE TABLE playlist_tracks (
        playlist_id INTEGER NOT NULL REFERENCES playlists (id) ON DELETE CASCADE,
        track_id    INTEGER NOT NULL REFERENCES tracks (id)    ON DELETE CASCADE,
        position    INTEGER NOT NULL,
        PRIMARY KEY (playlist_id, track_id)
      );

      CREATE INDEX idx_playlist_tracks_order
        ON playlist_tracks (playlist_id, position);
    `);
  },
];

/**
 * Opens the database and brings it up to the current schema version.
 * Safe to call repeatedly; subsequent calls return the same handle.
 */
export async function openDatabase(): Promise<SQLite.SQLiteDatabase> {
  if (handle !== null) return handle;

  const db = await SQLite.openDatabaseAsync(DATABASE_NAME);

  // Required for ON DELETE CASCADE, and off by default in SQLite.
  await db.execAsync('PRAGMA foreign_keys = ON;');
  // WAL keeps a long library scan from blocking reads by the UI.
  await db.execAsync('PRAGMA journal_mode = WAL;');

  const row = await db.getFirstAsync<{ user_version: number }>(
    'PRAGMA user_version;',
  );
  const current = row?.user_version ?? 0;

  for (let version = current; version < migrations.length; version += 1) {
    const migration = migrations[version];
    if (migration === undefined) break;
    await migration(db);
    // PRAGMA does not accept bound parameters, and `version` is a loop counter
    // over a literal array - never user input.
    await db.execAsync(`PRAGMA user_version = ${version + 1};`);
  }

  handle = db;
  return db;
}

/** Test/debug helper. Closing invalidates the cached handle. */
export async function closeDatabase(): Promise<void> {
  if (handle === null) return;
  await handle.closeAsync();
  handle = null;
}
