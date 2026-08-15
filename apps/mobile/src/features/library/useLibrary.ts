import { useCallback, useEffect, useState } from 'react';

import type { LibraryTrack } from '@ai-dj/core';

import { AiDjLibrary } from 'aidj-library';
import { listTracks, reconcileScan, type ScanResult } from '@/db/library';

export type LibraryState =
  | { status: 'idle' }
  | { status: 'needsPermission' }
  | { status: 'scanning' }
  | { status: 'ready'; tracks: LibraryTrack[]; lastScan: ScanResult | null }
  | { status: 'failed'; message: string };

/**
 * Owns the library list and the scan lifecycle.
 *
 * Stored tracks are shown immediately on mount; a scan only reconciles what
 * changed. That keeps the screen useful offline and on a cold start without
 * waiting for MediaStore.
 */
export function useLibrary() {
  const [state, setState] = useState<LibraryState>({ status: 'idle' });
  const [query, setQuery] = useState('');

  const load = useCallback(
    async (lastScan: ScanResult | null = null) => {
      try {
        const tracks = await listTracks({ query });
        setState({ status: 'ready', tracks, lastScan });
      } catch (error) {
        setState({
          status: 'failed',
          message:
            error instanceof Error ? error.message : 'Could not read the library.',
        });
      }
    },
    [query],
  );

  const scan = useCallback(async () => {
    setState({ status: 'scanning' });
    try {
      if (!AiDjLibrary.hasPermission()) {
        const granted = await AiDjLibrary.requestPermission();
        if (!granted) {
          setState({ status: 'needsPermission' });
          return;
        }
      }

      const scanned = await AiDjLibrary.scan();
      const result = await reconcileScan(scanned);
      await load(result);
    } catch (error) {
      setState({
        status: 'failed',
        message:
          error instanceof Error ? error.message : 'The library scan failed.',
      });
    }
  }, [load]);

  useEffect(() => {
    void load();
    // Re-runs on query change so search hits SQLite rather than filtering a
    // partial in-memory page.
  }, [load]);

  return { state, query, setQuery, scan, reload: load };
}
