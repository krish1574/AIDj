import { requireNativeModule } from 'expo-modules-core';

import type { ScannedTrack } from '@ai-dj/core';

/**
 * Typed facade over the MediaStore scanner.
 *
 * Scanning returns metadata only. Content hashing is a separate call because
 * it costs 2 MiB of I/O per track - see AiDjLibraryModule.kt for why identity
 * is computed lazily rather than during the scan.
 */
interface AiDjLibraryNativeModule {
  hasPermission(): boolean;
  requestPermission(): Promise<boolean>;
  scan(minDurationMs: number): Promise<ScannedTrack[]>;
  hashTrack(uri: string): Promise<string>;
}

const native = requireNativeModule<AiDjLibraryNativeModule>('AiDjLibrary');

/**
 * Below this, MediaStore's music collection is mostly ringtones, notification
 * sounds and voice-memo fragments. Nothing that short is a DJ-able track.
 *
 * Raised from 30 s after a real scan: a 30 s floor still admitted WhatsApp
 * voice notes, push-to-talk clips and screen-recording audio, which were
 * roughly half the short entries. 90 s is below the shortest plausible real
 * track while excluding essentially all of that.
 */
export const MIN_TRACK_DURATION_MS = 90_000;

export const AiDjLibrary = {
  hasPermission: () => native.hasPermission(),
  requestPermission: () => native.requestPermission(),
  scan: (minDurationMs: number = MIN_TRACK_DURATION_MS) =>
    native.scan(minDurationMs),
  hashTrack: (uri: string) => native.hashTrack(uri),
};
