import { requireNativeModule } from 'expo-modules-core';
import type { EventSubscription } from 'expo-modules-core';

/** Mirrors the result map built in AiDjAnalysisModule.kt. */
export interface NativeAnalysisResult {
  trackId: number;
  durationMs: number;
  bpm: number;
  bpmConfidence: number;
  /** Usually the half or double of `bpm`; see AnalysisTypes.h. */
  alternateBpm: number;
  beatConfidence: number;
  downbeatConfidence: number;
  beatsPerBar: number;
  /** 0 = C, 1 = C#, ... 11 = B. */
  keyTonic: number;
  /** 0 = major, 1 = minor. */
  keyMode: number;
  keyConfidence: number;
  integratedLufs: number;
  peakDbfs: number;
  energy: number;
  introEndMs: number;
  outroStartMs: number;
  analysisSampleRate: number;
  beatCount: number;
  beatsMs: number[];
  downbeatIndices: number[];
  sections: {
    startMs: number;
    endMs: number;
    energy: number;
    novelty: number;
  }[];
}

export interface AnalysisConditions {
  /** False when the device is throttling or the battery is low and unplugged. */
  allowed: boolean;
  batteryPercent: number;
  isCharging: boolean;
  thermallyThrottled: boolean;
}

export interface AnalysisProgressEvent {
  trackId: number;
  decodedMs: number;
  totalMs: number;
}

interface AiDjAnalysisNativeModule {
  canAnalyseNow(): AnalysisConditions;
  analyseTrack(trackId: number, uri: string): Promise<NativeAnalysisResult>;
  cancel(): Promise<void>;
  addListener(
    event: 'onAnalysisProgress',
    listener: (payload: AnalysisProgressEvent) => void,
  ): EventSubscription;
}

const native = requireNativeModule<AiDjAnalysisNativeModule>('AiDjAnalysis');

export const AiDjAnalysis = {
  /**
   * Whether conditions currently favour analysis. Check before queueing: the
   * native side also enforces this, but deciding in JS keeps the policy
   * visible and lets the UI explain why nothing is happening.
   */
  canAnalyseNow: (): AnalysisConditions => native.canAnalyseNow(),

  analyseTrack: (trackId: number, uri: string) =>
    native.analyseTrack(trackId, uri),

  cancel: () => native.cancel(),

  onProgress: (listener: (payload: AnalysisProgressEvent) => void) =>
    native.addListener('onAnalysisProgress', listener),
};
