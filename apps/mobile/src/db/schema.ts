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

/**
 * The in-flight open, not the finished handle.
 *
 * Caching the handle alone is a race: several modules call openDatabase() at
 * once during startup, all observe a null handle because it is only assigned
 * after migrations complete, and all run the migrations concurrently. Storing
 * the promise means the first caller does the work and everyone else awaits
 * the same result.
 */
let opening: Promise<SQLite.SQLiteDatabase> | null = null;

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

  // 2 - analysis cache
  async (db) => {
    await db.execAsync(`
      CREATE TABLE track_analysis (
        track_id             INTEGER PRIMARY KEY
                             REFERENCES tracks (id) ON DELETE CASCADE,
        analysis_version     INTEGER NOT NULL,
        -- Identity at the time of analysis. A file edited in place keeps its
        -- MediaStore id, so these are what actually invalidate the cache.
        size_bytes           INTEGER NOT NULL,
        modified_at_ms       INTEGER NOT NULL,

        duration_ms          REAL    NOT NULL,
        bpm                  REAL    NOT NULL,
        bpm_confidence       REAL    NOT NULL,
        alternate_bpm        REAL    NOT NULL,
        beat_confidence      REAL    NOT NULL,
        downbeat_confidence  REAL    NOT NULL,
        beats_per_bar        INTEGER NOT NULL,
        beat_count           INTEGER NOT NULL,
        key_tonic            INTEGER NOT NULL,
        key_mode             INTEGER NOT NULL,
        key_confidence       REAL    NOT NULL,
        integrated_lufs      REAL    NOT NULL,
        peak_dbfs            REAL    NOT NULL,
        energy               REAL    NOT NULL,
        intro_end_ms         REAL    NOT NULL,
        outro_start_ms       REAL    NOT NULL,
        analysis_sample_rate INTEGER NOT NULL,
        -- Beat grids are thousands of floats. Stored as a file on disk keyed
        -- by track id, not inline: SQLite would handle it, but a large blob
        -- per row bloats every query that does not need it.
        beat_grid_path       TEXT,
        sections_json        TEXT    NOT NULL,
        analysed_at_ms       INTEGER NOT NULL
      );

      CREATE INDEX idx_analysis_version ON track_analysis (analysis_version);

      -- Records tracks that could not be analysed, so a full-library pass does
      -- not retry the same unsupported or corrupt file on every run.
      CREATE TABLE analysis_failures (
        track_id       INTEGER PRIMARY KEY
                       REFERENCES tracks (id) ON DELETE CASCADE,
        reason         TEXT    NOT NULL,
        attempts       INTEGER NOT NULL DEFAULT 1,
        last_attempt_ms INTEGER NOT NULL
      );
    `);
  },
];

/**
 * Opens the database and brings it up to the current schema version.
 * Safe to call repeatedly; subsequent calls return the same handle.
 */
export function openDatabase(): Promise<SQLite.SQLiteDatabase> {
  if (opening !== null) return opening;

  opening = (async () => {
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
      // Each migration and its version bump go in one transaction, so a crash
      // partway through cannot leave the schema half-applied with a version
      // number claiming otherwise.
      await db.withTransactionAsync(async () => {
        await migration(db);
        // PRAGMA takes no bound parameters, and `version` is a loop counter
        // over a literal array - never user input.
        await db.execAsync(`PRAGMA user_version = ${version + 1};`);
      });
    }

    return db;
  })();

  // A failed open must not poison every later call with the same rejection.
  opening.catch(() => {
    opening = null;
  });

  return opening;
}

/** Test/debug helper. Closing invalidates the cached handle. */
export async function closeDatabase(): Promise<void> {
  if (opening === null) return;
  const db = await opening;
  opening = null;
  await db.closeAsync();
}
