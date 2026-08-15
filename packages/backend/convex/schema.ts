import { defineSchema, defineTable } from 'convex/server';
import { v } from 'convex/values';
import { authTables } from '@convex-dev/auth/server';

/**
 * Convex holds account-scoped metadata only. Audio never leaves the device.
 * Beat grids and vocal curves are float arrays in the kilobyte-to-hundreds-of-
 * kilobytes range and live in Convex file storage, referenced by id - putting
 * them in documents would blow the document size budget.
 *
 * Every user-owned table carries `userId` as its first index component and is
 * only ever read through a helper that derives the id from `ctx.auth`. A
 * client-supplied user id is never trusted.
 */

const musicalKey = v.object({
  tonic: v.number(),
  mode: v.union(v.literal('major'), v.literal('minor')),
});

const section = v.object({
  startMs: v.number(),
  endMs: v.number(),
  energy: v.number(),
  novelty: v.number(),
});

export default defineSchema({
  ...authTables,

  tracks: defineTable({
    userId: v.id('users'),
    contentHash: v.string(),
    title: v.string(),
    artist: v.union(v.string(), v.null()),
    album: v.union(v.string(), v.null()),
    durationMs: v.number(),
    mimeType: v.string(),
    sampleRate: v.number(),
    channelCount: v.number(),
    artworkStorageId: v.optional(v.id('_storage')),
  })
    // Upserts during a library scan look a track up by identity, not by id.
    .index('by_user_hash', ['userId', 'contentHash']),

  trackAnalysis: defineTable({
    userId: v.id('users'),
    trackId: v.id('tracks'),
    analysisVersion: v.number(),
    bpm: v.number(),
    bpmConfidence: v.number(),
    firstBeatMs: v.number(),
    beatCount: v.number(),
    downbeatIndices: v.array(v.number()),
    key: musicalKey,
    keyConfidence: v.number(),
    integratedLufs: v.number(),
    truePeakDbtp: v.number(),
    energy: v.number(),
    sections: v.array(section),
    introEndMs: v.number(),
    outroStartMs: v.number(),
    vocalActivityHopMs: v.number(),
    beatGridStorageId: v.id('_storage'),
    vocalCurveStorageId: v.id('_storage'),
  })
    // Version is part of the key so a bumped ANALYSIS_VERSION misses the cache
    // rather than silently returning stale results.
    .index('by_track_version', ['trackId', 'analysisVersion']),

  playlists: defineTable({
    userId: v.id('users'),
    name: v.string(),
    trackCount: v.number(),
  }).index('by_user', ['userId']),

  playlistTracks: defineTable({
    userId: v.id('users'),
    playlistId: v.id('playlists'),
    trackId: v.id('tracks'),
    position: v.number(),
  })
    .index('by_playlist_position', ['playlistId', 'position'])
    .index('by_track', ['trackId']),

  djSessions: defineTable({
    userId: v.id('users'),
    playlistId: v.id('playlists'),
    state: v.union(
      v.literal('active'),
      v.literal('paused'),
      v.literal('completed'),
      v.literal('abandoned'),
    ),
    mixStyle: v.union(
      v.literal('smooth'),
      v.literal('balanced'),
      v.literal('energetic'),
    ),
    startedAtMs: v.number(),
    endedAtMs: v.union(v.number(), v.null()),
  }).index('by_user', ['userId']),

  djQueueItems: defineTable({
    userId: v.id('users'),
    sessionId: v.id('djSessions'),
    position: v.number(),
    trackId: v.id('tracks'),
    plannerScore: v.number(),
    /** Per-term breakdown, kept so every ordering decision is explainable. */
    scoreBreakdown: v.record(v.string(), v.number()),
  }).index('by_session_position', ['sessionId', 'position']),

  mixTransitions: defineTable({
    userId: v.id('users'),
    sessionId: v.id('djSessions'),
    fromTrackId: v.id('tracks'),
    toTrackId: v.id('tracks'),
    transitionScore: v.number(),
    /** Set when the engine had to fall back; null on a clean transition. */
    degradedTo: v.union(v.string(), v.null()),
    executedAtMs: v.number(),
  }).index('by_session', ['sessionId']),

  userPreferences: defineTable({
    userId: v.id('users'),
    mixStyle: v.union(
      v.literal('smooth'),
      v.literal('balanced'),
      v.literal('energetic'),
    ),
    transitionLength: v.union(
      v.literal('short'),
      v.literal('medium'),
      v.literal('long'),
      // Around a minute at dance tempos: a long blend rather than a fade.
      v.literal('extended'),
    ),
    allowReordering: v.boolean(),
    targetLufs: v.number(),
  }).index('by_user', ['userId']),
});
