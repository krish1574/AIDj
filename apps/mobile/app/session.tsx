import { useCallback, useState } from 'react';
import { useFocusEffect, useLocalSearchParams } from 'expo-router';
import { ActivityIndicator, ScrollView, StyleSheet, Text, View } from 'react-native';

import type { LibraryTrack, MixStyle, TransitionLength } from '@ai-dj/core';
import { describeStep, formatDuration } from '@ai-dj/core';

import { Button } from '@/components/Button';
import { Panel, Row } from '@/components/Panel';
import { getAnalysis } from '@/db/analysis';
import { listTracks } from '@/db/library';
import { playlistTracks } from '@/db/playlists';
import { useDjSession } from '@/features/session/useDjSession';
import { theme } from '@/theme';

const STYLES: { value: MixStyle; label: string }[] = [
  { value: 'smooth', label: 'Smooth' },
  { value: 'balanced', label: 'Balanced' },
  { value: 'energetic', label: 'Energetic' },
];

const LENGTHS: { value: TransitionLength; label: string }[] = [
  { value: 'short', label: '4 bars' },
  { value: 'medium', label: '8 bars' },
  { value: 'long', label: '16 bars' },
  { value: 'extended', label: '32 bars' },
];

/**
 * The continuous DJ session.
 *
 * Everything else in the app exists to make this screen possible: pick a
 * playlist, press start, and the music keeps going without anyone touching it.
 */
export default function SessionScreen() {
  const params = useLocalSearchParams<{ playlistId?: string }>();
  const { state, start, stop } = useDjSession();

  const [tracks, setTracks] = useState<LibraryTrack[]>([]);
  const [analysedCount, setAnalysedCount] = useState(0);
  const [mixStyle, setMixStyle] = useState<MixStyle>('balanced');
  const [length, setLength] = useState<TransitionLength>('medium');

  const load = useCallback(async () => {
    const playlistId = params.playlistId ? Number(params.playlistId) : null;
    const list =
      playlistId !== null && Number.isFinite(playlistId)
        ? await playlistTracks(playlistId)
        : await listTracks({ limit: 100 });

    setTracks(list);

    let analysed = 0;
    for (const track of list) {
      if ((await getAnalysis(track.id)) !== null) analysed += 1;
    }
    setAnalysedCount(analysed);
  }, [params.playlistId]);

  useFocusEffect(
    useCallback(() => {
      void load();
    }, [load]),
  );

  if (state.status === 'playing') {
    const current = state.steps[state.currentIndex];
    const next = state.steps[state.currentIndex + 1];
    const upNext = state.steps.slice(state.currentIndex + 2, state.currentIndex + 5);

    return (
      <ScrollView contentContainerStyle={styles.content}>
        <Text style={styles.nowLabel}>
          {state.mixingNext ? 'MIXING INTO NEXT' : 'NOW PLAYING'}
        </Text>
        <Text style={styles.nowTitle} numberOfLines={2}>
          {current?.track.title ?? '—'}
        </Text>
        <Text style={styles.nowMeta}>
          {current ? `${current.track.bpm.toFixed(0)} BPM` : ''}
          {current ? ` · ${formatDuration(state.positionMs)}` : ''}
        </Text>

        {next !== undefined ? (
          <Panel title={state.mixingNext ? 'Mixing in' : 'Next'}>
            <Text style={styles.nextTitle} numberOfLines={1}>
              {next.track.title}
            </Text>
            <Text style={styles.muted}>{describeStep(next)}</Text>
          </Panel>
        ) : (
          <Panel title="Next">
            <Text style={styles.muted}>Last track of the set.</Text>
          </Panel>
        )}

        {upNext.length > 0 ? (
          <Panel title="Up next">
            {upNext.map((step) => (
              <Row
                key={step.track.id}
                label={step.track.title.slice(0, 24)}
                value={`${step.track.bpm.toFixed(0)} BPM`}
              />
            ))}
          </Panel>
        ) : null}

        <Button label="Stop" onPress={() => void stop()} variant="secondary" />
      </ScrollView>
    );
  }

  return (
    <ScrollView contentContainerStyle={styles.content}>
      {state.status === 'preparing' ? (
        <Panel title="Starting">
          <ActivityIndicator color={theme.colour.accent} />
          <Text style={styles.muted}>{state.detail}</Text>
        </Panel>
      ) : null}

      {state.status === 'failed' ? (
        <Panel title="Could not start">
          <Text style={styles.error}>{state.message}</Text>
        </Panel>
      ) : null}

      {state.status === 'complete' ? (
        <Panel title="Set finished">
          <Text style={styles.muted}>Every track played.</Text>
        </Panel>
      ) : null}

      <Panel title="Set">
        <Row label="Tracks" value={String(tracks.length)} />
        <Row
          label="Analysed"
          value={String(analysedCount)}
          tone={analysedCount === tracks.length ? 'positive' : 'warning'}
        />
        {analysedCount < tracks.length ? (
          <Text style={styles.muted}>
            Only analysed tracks can be mixed. The rest are skipped.
          </Text>
        ) : null}
      </Panel>

      <Panel title="Mix style">
        <View style={styles.options}>
          {STYLES.map((option) => (
            <Text
              key={option.value}
              onPress={() => setMixStyle(option.value)}
              style={[
                styles.option,
                mixStyle === option.value && styles.optionActive,
              ]}
            >
              {option.label}
            </Text>
          ))}
        </View>
        <Text style={styles.muted}>
          Style changes how willingly the set reorders your playlist and how
          much tempo and energy change it accepts between tracks.
        </Text>
      </Panel>

      <Panel title="Blend length">
        <View style={styles.options}>
          {LENGTHS.map((option) => (
            <Text
              key={option.value}
              onPress={() => setLength(option.value)}
              style={[
                styles.option,
                length === option.value && styles.optionActive,
              ]}
            >
              {option.label}
            </Text>
          ))}
        </View>
      </Panel>

      <Button
        label={analysedCount > 1 ? 'Start AI DJ' : 'Analyse tracks first'}
        onPress={() => void start(tracks, mixStyle, length)}
        disabled={analysedCount < 2 || state.status === 'preparing'}
      />

      <View style={styles.notice}>
        <Text style={styles.noticeText}>
          The set is planned before the first note: order, transition points,
          tempo changes and EQ moves are all decided up front and shown as it
          plays. Keep the app open - there is no background service yet, so
          playback stops if you leave.
        </Text>
      </View>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  content: { padding: theme.space.md, paddingBottom: theme.space.xl },
  nowLabel: {
    color: theme.colour.accent,
    fontSize: theme.type.label.fontSize,
    fontWeight: '700',
    letterSpacing: 1.5,
    marginBottom: theme.space.xs,
  },
  nowTitle: {
    color: theme.colour.text,
    fontSize: theme.type.display.fontSize,
    fontWeight: '700',
    marginBottom: theme.space.xs,
  },
  nowMeta: {
    color: theme.colour.textMuted,
    fontSize: theme.type.body.fontSize,
    fontFamily: 'monospace',
    marginBottom: theme.space.lg,
  },
  nextTitle: {
    color: theme.colour.text,
    fontSize: theme.type.body.fontSize,
    fontWeight: '600',
    marginBottom: theme.space.xs,
  },
  muted: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    lineHeight: 18,
    marginTop: theme.space.xs,
  },
  error: { color: theme.colour.negative, fontSize: theme.type.body.fontSize },
  options: { flexDirection: 'row', gap: theme.space.sm, flexWrap: 'wrap' },
  option: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    paddingVertical: theme.space.sm,
    paddingHorizontal: theme.space.md,
    borderRadius: theme.radius.sm,
    borderColor: theme.colour.border,
    borderWidth: StyleSheet.hairlineWidth,
    overflow: 'hidden',
  },
  optionActive: {
    color: theme.colour.text,
    backgroundColor: theme.colour.accentMuted,
    borderColor: theme.colour.accent,
  },
  notice: {
    marginTop: theme.space.lg,
    padding: theme.space.md,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colour.surface,
    borderLeftColor: theme.colour.accent,
    borderLeftWidth: 3,
  },
  noticeText: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    lineHeight: 19,
  },
});
