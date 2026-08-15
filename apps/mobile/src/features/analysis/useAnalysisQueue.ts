import { useCallback, useEffect, useRef, useState } from 'react';
import { AppState } from 'react-native';

import { AiDjAnalysis, type AnalysisConditions } from 'aidj-analysis';

import {
  analysisCoverage,
  recordFailure,
  saveAnalysis,
  tracksNeedingAnalysis,
  type AnalysisCoverage,
} from '@/db/analysis';

export type QueueState =
  | { status: 'idle' }
  | { status: 'running'; title: string; trackId: number; progress: number }
  | { status: 'paused'; reason: string }
  | { status: 'complete' };

/**
 * Drives whole-library analysis, one track at a time.
 *
 * Deliberately sequential. Parallel analysis would not finish sooner - the
 * work is CPU-bound on the same few cores - and it would multiply peak memory
 * and heat while making every individual track take longer to become usable.
 *
 * The loop re-checks device conditions before each track and stops when the
 * phone is hot or the battery is low and unplugged, because the user is not
 * waiting on this work and a hot phone is a worse outcome than a slow queue.
 */
export function useAnalysisQueue() {
  const [state, setState] = useState<QueueState>({ status: 'idle' });
  const [coverage, setCoverage] = useState<AnalysisCoverage>({
    total: 0,
    analysed: 0,
    failed: 0,
  });

  // Ref rather than state: the loop reads it every iteration and must see the
  // current value, not the one captured when the closure was created.
  const runningRef = useRef(false);

  const refreshCoverage = useCallback(async () => {
    setCoverage(await analysisCoverage());
  }, []);

  useEffect(() => {
    void refreshCoverage();
  }, [refreshCoverage]);

  useEffect(() => {
    const subscription = AiDjAnalysis.onProgress((event) => {
      setState((current) =>
        current.status === 'running' && current.trackId === event.trackId
          ? {
              ...current,
              progress:
                event.totalMs > 0 ? event.decodedMs / event.totalMs : 0,
            }
          : current,
      );
    });
    return () => subscription.remove();
  }, []);

  const stop = useCallback(() => {
    runningRef.current = false;
    void AiDjAnalysis.cancel();
    setState({ status: 'idle' });
  }, []);

  // Analysis is foreground-only for now: there is no foreground service, so
  // Android will freeze this work when the app is backgrounded anyway. Better
  // to stop cleanly than to be killed mid-track.
  useEffect(() => {
    const subscription = AppState.addEventListener('change', (next) => {
      if (next !== 'active' && runningRef.current) stop();
    });
    return () => subscription.remove();
  }, [stop]);

  const describeBlock = (conditions: AnalysisConditions): string => {
    if (conditions.thermallyThrottled) {
      return 'Paused: your phone is warm. Analysis will use less power once it cools down.';
    }
    return `Paused: battery at ${conditions.batteryPercent}%. Plug in to continue analysing.`;
  };

  const start = useCallback(async () => {
    if (runningRef.current) return;
    runningRef.current = true;

    try {
      while (runningRef.current) {
        const conditions = AiDjAnalysis.canAnalyseNow();
        if (!conditions.allowed) {
          setState({ status: 'paused', reason: describeBlock(conditions) });
          break;
        }

        const pending = await tracksNeedingAnalysis(1);
        const track = pending[0];
        if (track === undefined) {
          setState({ status: 'complete' });
          break;
        }

        setState({
          status: 'running',
          title: track.title,
          trackId: track.id,
          progress: 0,
        });

        try {
          const result = await AiDjAnalysis.analyseTrack(
            track.id,
            track.contentUri,
          );
          await saveAnalysis(track, result);
        } catch (error) {
          // A failure here is usually an unsupported codec or a file that has
          // gone away. Recorded so the queue does not retry it forever, and
          // the loop continues rather than stopping the whole pass.
          await recordFailure(
            track.id,
            error instanceof Error ? error.message : 'Unknown error',
          );
        }

        await refreshCoverage();
      }
    } finally {
      runningRef.current = false;
    }
  }, [refreshCoverage]);

  return { state, coverage, start, stop, refreshCoverage };
}
