import { useCallback, useState } from 'react';

import type { LibraryTrack, TransitionPlan, TransitionTrack } from '@ai-dj/core';
import { describeTransition, planTransition } from '@ai-dj/core';

import { AiDjAudio } from 'aidj-audio';
import { getAnalysis, readBeatGrid } from '@/db/analysis';

export type DemoState =
  | { status: 'idle' }
  | { status: 'preparing'; detail: string }
  | { status: 'playing'; plan: TransitionPlan; summary: string; firesInMs: number }
  | { status: 'transitioning'; plan: TransitionPlan; summary: string }
  | { status: 'done'; plan: TransitionPlan; summary: string }
  | { status: 'failed'; message: string };

/**
 * Builds the planner's view of a track from stored analysis.
 *
 * Returns null when a track has not been analysed: planning a transition from
 * guessed numbers would produce a confident-looking plan built on nothing.
 */
async function toTransitionTrack(
  track: LibraryTrack,
): Promise<TransitionTrack | null> {
  const analysis = await getAnalysis(track.id);
  if (analysis === null) return null;

  const beats =
    analysis.beatGridPath !== null
      ? Array.from(await readBeatGrid(analysis.beatGridPath))
      : [];

  return {
    id: track.id,
    title: track.title,
    durationMs: analysis.durationMs,
    bpm: analysis.bpm,
    beatConfidence: analysis.beatConfidence,
    beatsMs: beats,
    downbeatIndices: analysis.downbeatIndices,
    beatsPerBar: analysis.beatsPerBar,
    introEndMs: analysis.introEndMs,
    outroStartMs: analysis.outroStartMs,
    integratedLufs: analysis.integratedLufs,
  };
}

/**
 * Runs one real transition between two analysed tracks.
 *
 * A development harness, not the product: it plays a fixed pair on demand
 * rather than working through a queue. It exists because every part of the
 * transition engine has been verified by measurement and none of it has been
 * heard, and that is the only test that decides whether this sounds like a DJ.
 */
export function useTransitionDemo() {
  const [state, setState] = useState<DemoState>({ status: 'idle' });

  const run = useCallback(
    async (outgoing: LibraryTrack, incoming: LibraryTrack) => {
      try {
        setState({ status: 'preparing', detail: 'Reading analysis…' });

        const from = await toTransitionTrack(outgoing);
        const to = await toTransitionTrack(incoming);

        if (from === null || to === null) {
          setState({
            status: 'failed',
            message:
              'Both tracks need to be analysed first. Run the analysis screen.',
          });
          return;
        }

        const plan = planTransition(from, to);
        const summary = describeTransition(plan);

        setState({ status: 'preparing', detail: 'Cueing tracks…' });

        await AiDjAudio.initialise();

        // Outgoing track starts a few seconds before the transition point, so
        // there is something playing to mix out of rather than the fade
        // starting from silence.
        const leadInMs = 8000;
        const outgoingCueMs = Math.max(0, plan.outgoingStartMs - leadInMs);

        await AiDjAudio.prepareVoice(
          0,
          outgoing.contentUri,
          outgoingCueMs,
          plan.outgoingTempoRatio,
        );
        await AiDjAudio.prepareVoice(
          1,
          incoming.contentUri,
          plan.incomingStartMs,
          plan.incomingTempoRatio,
        );

        // Both voices must hold audio before anything starts, or the first
        // moments are silence while the decoders catch up.
        await new Promise((resolve) => setTimeout(resolve, 1200));

        await AiDjAudio.playVoice(0);

        const firesInMs = plan.outgoingStartMs - outgoingCueMs;
        await AiDjAudio.armTransition(plan, 0, 1, firesInMs);

        setState({ status: 'playing', plan, summary, firesInMs });

        // Poll for completion rather than assuming the timing worked: the
        // engine is the authority on when the transition actually ended.
        const before = AiDjAudio.transitionsCompleted();
        const deadline = Date.now() + firesInMs + plan.durationMs + 5000;
        const poll = setInterval(() => {
          if (AiDjAudio.transitionsCompleted() > before) {
            clearInterval(poll);
            setState({ status: 'done', plan, summary });
          } else if (Date.now() > deadline) {
            clearInterval(poll);
            setState({
              status: 'failed',
              message: 'The transition did not complete within the expected time.',
            });
          } else {
            setState((current) =>
              current.status === 'playing' &&
              Date.now() > deadline - plan.durationMs - 5000
                ? { status: 'transitioning', plan, summary }
                : current,
            );
          }
        }, 250);
      } catch (error) {
        setState({
          status: 'failed',
          message: error instanceof Error ? error.message : 'Unknown error',
        });
      }
    },
    [],
  );

  const stop = useCallback(async () => {
    await AiDjAudio.clearTransition();
    await AiDjAudio.stopAll();
    setState({ status: 'idle' });
  }, []);

  return { state, run, stop };
}
