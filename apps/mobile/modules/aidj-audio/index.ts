import { requireNativeModule } from 'expo-modules-core';

import type { EngineStatus, TransitionPlan, VoiceIndex } from '@ai-dj/core';

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
  prepareVoice(
    voiceIndex: number,
    uri: string,
    startMs: number,
    tempoRatio: number,
  ): Promise<void>;
  armTransition(
    spec: number[],
    outgoingVoice: number,
    incomingVoice: number,
    delayMs: number,
  ): Promise<void>;
  clearTransition(): Promise<void>;
  transitionsCompleted(): number;
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

  /**
   * Loads a voice cued to `startMs` and pre-stretched to `tempoRatio`.
   * The tempo must be set here rather than at transition time: audio already
   * in the ring keeps the rate it was decoded at.
   */
  prepareVoice: (
    voice: VoiceIndex,
    uri: string,
    startMs: number,
    tempoRatio: number,
  ) => native.prepareVoice(voice, uri, startMs, tempoRatio),

  /**
   * Arms a transition planned by `planTransition` from @ai-dj/core.
   *
   * `delayMs` is how far ahead it should begin, which is how the transition is
   * made to land on a chosen beat rather than whenever the call is processed.
   */
  armTransition: (
    plan: TransitionPlan,
    outgoingVoice: VoiceIndex,
    incomingVoice: VoiceIndex,
    delayMs: number,
  ) =>
    native.armTransition(
      transitionPlanToSpec(plan),
      outgoingVoice,
      incomingVoice,
      delayMs,
    ),

  clearTransition: () => native.clearTransition(),

  /** Increments each time a transition finishes. Poll to detect completion. */
  transitionsCompleted: (): number => native.transitionsCompleted(),

  getStatus: (): EngineStatus => native.getStatus(),
};

/**
 * Flattens a plan into the array layout the native side expects.
 *
 * The order is fixed and mirrored in three places - here, TransitionSpecFields
 * in NativeEngine.kt, and TransitionFields in JniBridge.cpp. Kept as a single
 * function so there is exactly one place to change it.
 */
export function transitionPlanToSpec(plan: TransitionPlan): number[] {
  return [
    plan.durationMs,
    plan.incomingStartMs,
    plan.outgoingGain,
    plan.incomingGain,
    plan.outgoingTempoRatio,
    plan.incomingTempoRatio,
    plan.eq.outgoing.lowFrom,
    plan.eq.outgoing.lowTo,
    plan.eq.outgoing.midFrom,
    plan.eq.outgoing.midTo,
    plan.eq.incoming.lowFrom,
    plan.eq.incoming.lowTo,
    plan.eq.incoming.midFrom,
    plan.eq.incoming.midTo,
    // Every style except the gapless fallback overlaps the two tracks.
    plan.style === 'gapless' ? 0 : 1,
  ];
}

export type { EngineStatus, TransitionPlan, VoiceIndex };
