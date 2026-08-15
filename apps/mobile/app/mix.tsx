import { useCallback, useState } from 'react';
import { useFocusEffect } from 'expo-router';
import { FlatList, StyleSheet, Text, View } from 'react-native';

import type { LibraryTrack, TransitionLength } from '@ai-dj/core';

import { Button } from '@/components/Button';
import { Panel, Row } from '@/components/Panel';
import { TrackRow } from '@/components/TrackRow';
import { getAnalysis } from '@/db/analysis';
import { listTracks } from '@/db/library';
import { useTransitionDemo } from '@/features/transition/useTransitionDemo';
import { theme } from '@/theme';

/**
 * Development harness for hearing one real transition.
 *
 * Not the product: it mixes a chosen pair on demand rather than working
 * through a queue. It exists because every part of the transition engine has
 * been verified by measurement and none of it has been heard, and listening is
 * the only test that settles whether this sounds like a DJ.
 */
export default function MixScreen() {
  const { state, run, stop } = useTransitionDemo();
  const [tracks, setTracks] = useState<LibraryTrack[]>([]);
  const [selected, setSelected] = useState<LibraryTrack[]>([]);
  const [length, setLength] = useState<TransitionLength>('medium');

  const load = useCallback(async () => {
    const all = await listTracks({ limit: 200 });
    const analysed: LibraryTrack[] = [];
    for (const track of all) {
      if ((await getAnalysis(track.id)) !== null) analysed.push(track);
    }
    setTracks(analysed);
  }, []);

  useFocusEffect(
    useCallback(() => {
      void load();
    }, [load]),
  );

  const toggle = useCallback((track: LibraryTrack) => {
    setSelected((current) => {
      if (current.some((item) => item.id === track.id)) {
        return current.filter((item) => item.id !== track.id);
      }
      // Keep the two most recent picks: first is mixed out of, second into.
      return [...current, track].slice(-2);
    });
  }, []);

  const canMix = selected.length === 2 && state.status !== 'preparing';

  return (
    <View style={styles.container}>
      <FlatList
        data={tracks}
        keyExtractor={(track) => String(track.id)}
        contentContainerStyle={styles.list}
        ListHeaderComponent={
          <View>
            {state.status === 'failed' ? (
              <Panel title="Failed">
                <Text style={styles.error}>{state.message}</Text>
              </Panel>
            ) : null}

            {state.status === 'preparing' ? (
              <Panel title="Preparing">
                <Text style={styles.muted}>{state.detail}</Text>
              </Panel>
            ) : null}

            {(state.status === 'playing' ||
              state.status === 'transitioning' ||
              state.status === 'done') ? (
              <Panel title={state.status === 'done' ? 'Finished' : 'Mixing'}>
                <Text style={styles.summary}>{state.summary}</Text>
                <Row label="Style" value={state.plan.style} />
                <Row
                  label="Target tempo"
                  value={`${state.plan.targetBpm.toFixed(1)} BPM`}
                />
                <Row
                  label="Incoming stretch"
                  value={`${((state.plan.incomingTempoRatio - 1) * 100).toFixed(2)}%`}
                />
                <Row
                  label="Length"
                  value={`${(state.plan.durationMs / 1000).toFixed(1)} s`}
                />
                <Row
                  label="Quality"
                  value={state.plan.score.total.toFixed(2)}
                  tone={state.plan.score.total > 0.6 ? 'positive' : 'warning'}
                />
                {state.plan.fallbackReason !== undefined ? (
                  <Text style={styles.warning}>{state.plan.fallbackReason}</Text>
                ) : null}
              </Panel>
            ) : null}

            <Panel title="Blend length">
              <View style={styles.lengths}>
                {(['short', 'medium', 'long', 'extended'] as const).map(
                  (option) => (
                    <Text
                      key={option}
                      onPress={() => setLength(option)}
                      style={[
                        styles.lengthOption,
                        length === option && styles.lengthOptionActive,
                      ]}
                    >
                      {option === 'short'
                        ? '4 bars'
                        : option === 'medium'
                          ? '8 bars'
                          : option === 'long'
                            ? '16 bars'
                            : '32 bars'}
                    </Text>
                  ),
                )}
              </View>
              <Text style={styles.muted}>
                Measured in bars, so it lands on a bar line at any tempo. At
                126 BPM that is roughly 8, 15, 30 and 60 seconds. Over a long
                blend, small tempo errors accumulate - a 0.1% error drifts a
                quarter of a beat across a minute.
              </Text>
            </Panel>

            {tracks.length === 0 ? (
              <Text style={styles.muted}>
                No analysed tracks yet. Run the analysis screen first - a
                transition cannot be planned without beat grids.
              </Text>
            ) : (
              <Text style={styles.muted}>
                Pick two tracks. The first is mixed out of, the second into.
              </Text>
            )}
          </View>
        }
        renderItem={({ item }) => {
          const position = selected.findIndex((track) => track.id === item.id);
          return (
            <TrackRow
              track={item}
              selected={position >= 0}
              onPress={() => toggle(item)}
              {...(position >= 0 ? { index: position } : {})}
            />
          );
        }}
      />

      <View style={styles.footer}>
        {state.status === 'playing' || state.status === 'transitioning' ? (
          <Button label="Stop" onPress={() => void stop()} variant="secondary" />
        ) : (
          <Button
            label={
              selected.length === 2
                ? `Mix "${selected[0]?.title.slice(0, 14)}" into "${selected[1]?.title.slice(0, 14)}"`
                : 'Select two tracks'
            }
            onPress={() => {
              const [outgoing, incoming] = selected;
              if (outgoing && incoming) void run(outgoing, incoming, length);
            }}
            disabled={!canMix}
          />
        )}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: theme.colour.background },
  list: { padding: theme.space.md },
  footer: {
    padding: theme.space.md,
    borderTopColor: theme.colour.border,
    borderTopWidth: StyleSheet.hairlineWidth,
  },
  muted: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    lineHeight: 18,
    marginBottom: theme.space.md,
  },
  summary: {
    color: theme.colour.text,
    fontSize: theme.type.body.fontSize,
    marginBottom: theme.space.sm,
  },
  error: { color: theme.colour.negative, fontSize: theme.type.body.fontSize },
  lengths: {
    flexDirection: 'row',
    gap: theme.space.sm,
    marginBottom: theme.space.sm,
  },
  lengthOption: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    paddingVertical: theme.space.sm,
    paddingHorizontal: theme.space.md,
    borderRadius: theme.radius.sm,
    borderColor: theme.colour.border,
    borderWidth: StyleSheet.hairlineWidth,
    overflow: 'hidden',
  },
  lengthOptionActive: {
    color: theme.colour.text,
    backgroundColor: theme.colour.accentMuted,
    borderColor: theme.colour.accent,
  },
  warning: {
    color: theme.colour.warning,
    fontSize: theme.type.label.fontSize,
    marginTop: theme.space.sm,
    lineHeight: 18,
  },
});
