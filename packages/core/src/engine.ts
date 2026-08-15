/**
 * The contract between the TypeScript layer and the native audio engine.
 *
 * The authoritative playback state lives in C++, next to the sample clock.
 * TypeScript holds a read-only mirror updated by events. There are no
 * `isPlaying` / `isTransitioning` booleans anywhere in this codebase - the UI
 * branches on `EngineState` alone.
 */

export const ENGINE_SAMPLE_RATE = 48_000;
export const ENGINE_CHANNEL_COUNT = 2;

/** Two decoder voices. Milestone 1 exposes both; the mixer sums them. */
export const VOICE_COUNT = 2;
export type VoiceIndex = 0 | 1;

export type EngineState =
  | 'uninitialised'
  | 'idle'
  | 'preparing'
  | 'playing'
  | 'paused'
  | 'error';

export interface VoiceStatus {
  index: number;
  /** False when no source is loaded into this voice. */
  hasSource: boolean;
  /** Playhead in the source track, milliseconds. */
  positionMs: number;
  durationMs: number;
  /** Linear gain currently applied by the mixer, 0..1. */
  gain: number;
  /** True once the decoder has buffered enough to start without underrunning. */
  primed: boolean;
  /** True once the source has been decoded to its end. */
  endOfStream: boolean;
}

export interface EngineStatus {
  state: EngineState;
  voices: readonly VoiceStatus[];
  /**
   * Oboe XRun count since stream start. Non-zero means the audio callback
   * missed a deadline and the user heard a glitch. This is the single most
   * important health metric for the whole product; it is surfaced in the debug
   * screen and asserted on in device testing.
   */
  underrunCount: number;
  /** Actual output buffer size chosen by Oboe, frames. */
  framesPerBurst: number;
  /** Measured output latency in milliseconds, if the device reports it. */
  outputLatencyMs: number | null;
  /**
   * Frames the mixer wanted from an active voice that the decoder had not
   * produced yet. Distinct from `underrunCount`: this is our pipeline falling
   * behind, that is the device's audio stream missing its deadline.
   */
  starvedFrames: number;
  /** 'NONE' when healthy. */
  lastError: EngineErrorCode;
}

export type EngineErrorCode =
  | 'NONE'
  | 'OUTPUT_OPEN_FAILED'
  | 'DECODER_UNSUPPORTED_FORMAT'
  | 'DECODER_IO_ERROR'
  | 'FILE_NOT_FOUND'
  | 'VOICE_BUSY'
  | 'INVALID_STATE';

/**
 * Milestone 1 only. A manual, fixed-duration, equal-power crossfade with no
 * beat alignment, no tempo matching and no EQ automation. It exists to prove
 * that gain automation reaches the audio callback sample-accurately and that
 * the two-voice summing path is correct under real threading.
 *
 * It is NOT the transition engine and is NOT reachable from the product UI -
 * only from the developer debug screen. The real transition engine is
 * Milestone 5 and requires the beat grids that Milestone 3 produces.
 */
export interface DevCrossfadeRequest {
  fromVoice: VoiceIndex;
  toVoice: VoiceIndex;
  durationMs: number;
}
