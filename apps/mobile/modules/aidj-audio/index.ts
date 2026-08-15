import { requireNativeModule } from 'expo-modules-core';

import type { EngineStatus, VoiceIndex } from '@ai-dj/core';

/**
 * Typed facade over the native audio engine.
 *
 * The native side is the single source of truth for playback state; this
 * module never caches it. `getStatus()` is a synchronous read of the real
 * engine, not a mirror that can drift.
 */
interface AiDjAudioNativeModule {
  readonly sampleRate: number;
  readonly channelCount: number;
  readonly voiceCount: number;

  initialise(): Promise<void>;
  shutdown(): Promise<void>;
  loadVoice(voiceIndex: number, uri: string): Promise<void>;
  playVoice(voiceIndex: number): Promise<void>;
  pause(): Promise<void>;
  resume(): Promise<void>;
  stopAll(): Promise<void>;
  devCrossfade(
    fromVoice: number,
    toVoice: number,
    durationMs: number,
  ): Promise<void>;
  getStatus(): EngineStatus;
}

const native = requireNativeModule<AiDjAudioNativeModule>('AiDjAudio');

export const AiDjAudio = {
  sampleRate: native.sampleRate,
  channelCount: native.channelCount,
  voiceCount: native.voiceCount,

  initialise: () => native.initialise(),
  shutdown: () => native.shutdown(),
  loadVoice: (voice: VoiceIndex, uri: string) => native.loadVoice(voice, uri),
  playVoice: (voice: VoiceIndex) => native.playVoice(voice),
  pause: () => native.pause(),
  resume: () => native.resume(),
  stopAll: () => native.stopAll(),

  /**
   * Milestone 1 developer tool. A fixed-length equal-power crossfade with no
   * beat alignment, tempo matching or EQ - it proves the automation path, it
   * is not the transition engine.
   */
  devCrossfade: (from: VoiceIndex, to: VoiceIndex, durationMs: number) =>
    native.devCrossfade(from, to, durationMs),

  getStatus: (): EngineStatus => native.getStatus(),
};

export type { EngineStatus, VoiceIndex };
