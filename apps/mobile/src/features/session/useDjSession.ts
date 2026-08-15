import { useCallback, useEffect, useRef, useState } from 'react';

import type { LibraryTrack, SessionStep, SessionTrack } from '@ai-dj/core';
import {
  DEFAULT_SESSION_OPTIONS,
  buildSession,
  msUntilTransition,
  shouldPrepareNext,
  type MixStyle,
  type TransitionLength,
} from '@ai-dj/core';

import { AiDjAudio } from 'aidj-audio';
import { getAnalysis, readBeatGrid } from '@/db/analysis';

export type SessionState =
  | { status: 'idle' }
  | { status: 'preparing'; detail: string }
  | {
      status: 'playing';
      steps: SessionStep[];
      currentIndex: number;
      /** Playhead within the current track, in that track's own timeline. */
      positionMs: number;
      mixingNext: boolean;
    }
  | { status: 'complete' }
  | { status: 'failed'; message: string };

/** Poll interval for the playhead. Fast enough to arm on time, cheap enough. */
const TICK_MS = 250;

/**
 * Drives a continuous DJ session.
 *
 * Contains no musical decisions - those all live in @ai-dj/core, where they
 * are testable without a device. This is the part that talks to the engine:
 * it watches the playhead, prepares the next track in time, arms each
 * transition, and moves on when one completes.
 *
 * The two-voice constraint shapes everything. A track can only be prepared
 * once the voice it needs is free, which is why preparation looks exactly one
 * track ahead and no further.
 */
export function useDjSession() {
  const [state, setState] = useState<SessionState>({ status: 'idle' });

  // Refs rather than state: the tick reads these every 250 ms and must see
  // current values, not the ones captured when its closure was created.
  const stepsRef = useRef<SessionStep[]>([]);
  const indexRef = useRef(0);
  const preparedRef = useRef(-1);
  const armedRef = useRef(-1);
  const cueOffsetRef = useRef(0);
  const completionsRef = useRef(0);
  const runningRef = useRef(false);

  const stop = useCallback(async () => {
    runningRef.current = false;
    await AiDjAudio.clearTransition();
    await AiDjAudio.stopAll();
    setState({ status: 'idle' });
  }, []);

  useEffect(() => {
    return () => {
      runningRef.current = false;
    };
  }, []);

  /** Reads stored analysis into the shape both planners need. */
  const toSessionTrack = useCallback(
    async (track: LibraryTrack): Promise<SessionTrack | null> => {
      const analysis = await getAnalysis(track.id);
      if (analysis === null) return null;

      const beats =
        analysis.beatGridPath !== null
          ? Array.from(await readBeatGrid(analysis.beatGridPath))
          : [];

      return {
        id: track.id,
        title: track.title,
        contentUri: track.contentUri,
        durationMs: analysis.durationMs,
        bpm: analysis.bpm,
        bpmConfidence: analysis.bpmConfidence,
        alternateBpm: analysis.alternateBpm,
        beatConfidence: analysis.beatConfidence,
        beatsMs: beats,
        downbeatIndices: analysis.downbeatIndices,
        beatsPerBar: analysis.beatsPerBar,
        introEndMs: analysis.introEndMs,
        outroStartMs: analysis.outroStartMs,
        integratedLufs: analysis.integratedLufs,
        sections: analysis.sections,
        energy: analysis.energy,
        key: {
          tonic: analysis.keyTonic,
          mode: analysis.keyMode === 0 ? 'major' : 'minor',
        },
        keyConfidence: analysis.keyConfidence,
        // Not stored per track yet; the planner weights it low.
        vocalActivity: 0.5,
      };
    },
    [],
  );

  const start = useCallback(
    async (
      tracks: readonly LibraryTrack[],
      mixStyle: MixStyle = 'balanced',
      length: TransitionLength = 'medium',
    ) => {
      if (runningRef.current) return;

      try {
        setState({ status: 'preparing', detail: 'Reading analysis…' });

        const analysed: SessionTrack[] = [];
        for (const track of tracks) {
          const sessionTrack = await toSessionTrack(track);
          if (sessionTrack !== null) analysed.push(sessionTrack);
        }

        if (analysed.length === 0) {
          setState({
            status: 'failed',
            message:
              'None of these tracks have been analysed yet. Run the analysis screen first.',
          });
          return;
        }

        setState({ status: 'preparing', detail: 'Planning the set…' });

        const steps = buildSession(analysed, {
          ...DEFAULT_SESSION_OPTIONS,
          mixStyle,
          transition: { length, adaptIncoming: true },
        });

        stepsRef.current = steps;
        indexRef.current = 0;
        preparedRef.current = -1;
        armedRef.current = -1;
        completionsRef.current = 0;

        const first = steps[0];
        if (first === undefined) {
          setState({ status: 'failed', message: 'The set came out empty.' });
          return;
        }

        setState({ status: 'preparing', detail: `Cueing ${first.track.title}…` });

        await AiDjAudio.initialise();
        // The opener starts from where its music actually begins, so a set
        // never opens with silence.
        cueOffsetRef.current = first.track.introEndMs;
        await AiDjAudio.prepareVoice(
          first.voice,
          first.track.contentUri,
          first.track.introEndMs,
          1,
        );

        // Let the ring fill before starting, or the first seconds stutter.
        await new Promise((resolve) => setTimeout(resolve, 1200));
        await AiDjAudio.playVoice(first.voice);

        completionsRef.current = AiDjAudio.transitionsCompleted();
        runningRef.current = true;

        setState({
          status: 'playing',
          steps,
          currentIndex: 0,
          positionMs: first.track.introEndMs,
          mixingNext: false,
        });
      } catch (error) {
        setState({
          status: 'failed',
          message: error instanceof Error ? error.message : 'Unknown error',
        });
      }
    },
    [toSessionTrack],
  );

  // The pipeline. Runs on a timer while a session is playing.
  useEffect(() => {
    if (state.status !== 'playing') return undefined;

    const timer = setInterval(() => {
      if (!runningRef.current) return;

      void (async () => {
        const steps = stepsRef.current;
        const index = indexRef.current;
        const current = steps[index];
        const next = steps[index + 1];
        if (current === undefined) return;

        const status = AiDjAudio.getStatus();
        const voice = status.voices[current.voice];
        if (voice === undefined) return;

        // The engine reports position from the cue point, not from the start
        // of the file, so the track's own timeline needs the offset added.
        const positionMs = cueOffsetRef.current + voice.positionMs;

        // A completed transition means the next track is now the current one.
        const completions = AiDjAudio.transitionsCompleted();
        if (completions > completionsRef.current) {
          completionsRef.current = completions;
          indexRef.current = index + 1;

          const promoted = steps[index + 1];
          if (promoted === undefined) {
            runningRef.current = false;
            setState({ status: 'complete' });
            return;
          }

          cueOffsetRef.current = promoted.transition?.incomingStartMs ?? 0;
          setState({
            status: 'playing',
            steps,
            currentIndex: index + 1,
            positionMs: cueOffsetRef.current,
            mixingNext: false,
          });
          return;
        }

        if (next === undefined || next.transition === null) {
          // Last track: let it play out rather than cutting it off.
          setState((previous) =>
            previous.status === 'playing'
              ? { ...previous, positionMs }
              : previous,
          );
          if (voice.endOfStream && voice.positionMs > 0) {
            runningRef.current = false;
            setState({ status: 'complete' });
          }
          return;
        }

        // Prepare the next track early enough that a slow file or a throttled
        // CPU cannot make the transition wait on I/O.
        if (
          preparedRef.current !== index + 1 &&
          shouldPrepareNext(positionMs, next.transition)
        ) {
          preparedRef.current = index + 1;
          await AiDjAudio.prepareVoice(
            next.voice,
            next.track.contentUri,
            next.transition.incomingStartMs,
            next.transition.incomingTempoRatio,
          );
        }

        // Arm the transition once, ahead of its moment, so the engine fires it
        // on the exact frame rather than whenever this timer happens to run.
        const remaining = msUntilTransition(positionMs, next.transition);
        if (
          armedRef.current !== index + 1 &&
          preparedRef.current === index + 1 &&
          remaining <= TICK_MS * 8
        ) {
          armedRef.current = index + 1;
          await AiDjAudio.armTransition(
            next.transition,
            current.voice,
            next.voice,
            Math.max(0, remaining),
          );
        }

        setState((previous) =>
          previous.status === 'playing'
            ? { ...previous, positionMs, mixingNext: armedRef.current === index + 1 }
            : previous,
        );
      })();
    }, TICK_MS);

    return () => clearInterval(timer);
  }, [state.status]);

  return { state, start, stop };
}
