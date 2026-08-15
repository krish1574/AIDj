import { useCallback, useState } from 'react';
import { useFocusEffect } from 'expo-router';
import { FlatList, StyleSheet, Text, View } from 'react-native';

import { formatDuration, type LibraryTrack } from '@ai-dj/core';

import { Button } from '@/components/Button';
import { Panel, Row } from '@/components/Panel';
import { getAnalysis, type StoredAnalysis } from '@/db/analysis';
import { listTracks } from '@/db/library';
import { useAnalysisQueue } from '@/features/analysis/useAnalysisQueue';
import { theme } from '@/theme';

const PITCH_NAMES = [
  'C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B',
] as const;

function describeKey(tonic: number, mode: number): string {
  const name = PITCH_NAMES[tonic] ?? '?';
  return `${name} ${mode === 0 ? 'major' : 'minor'}`;
}

/** Confidence is load-bearing, so it is shown rather than hidden. */
function confidenceTone(value: number): 'positive' | 'warning' | 'negative' {
  if (value >= 0.5) return 'positive';
  if (value >= 0.2) return 'warning';
  return 'negative';
}

interface AnalysedRow {
  track: LibraryTrack;
  analysis: StoredAnalysis;
}

export default function AnalysisScreen() {
  const { state, coverage, start, stop, refreshCoverage } = useAnalysisQueue();
  const [rows, setRows] = useState<AnalysedRow[]>([]);

  const load = useCallback(async () => {
    await refreshCoverage();
    const tracks = await listTracks({ limit: 200 });
    const analysed: AnalysedRow[] = [];
    for (const track of tracks) {
      const analysis = await getAnalysis(track.id);
      if (analysis !== null) analysed.push({ track, analysis });
    }
    setRows(analysed);
  }, [refreshCoverage]);

  useFocusEffect(
    useCallback(() => {
      void load();
    }, [load]),
  );

  const remaining = coverage.total - coverage.analysed - coverage.failed;

  return (
    <View style={styles.container}>
      <FlatList
        data={rows}
        keyExtractor={(row) => String(row.track.id)}
        contentContainerStyle={styles.list}
        ListHeaderComponent={
          <View>
            <Panel title="Coverage">
              <Row label="Tracks" value={String(coverage.total)} />
              <Row
                label="Analysed"
                value={String(coverage.analysed)}
                tone={coverage.analysed > 0 ? 'positive' : 'default'}
              />
              <Row label="Remaining" value={String(remaining)} />
              {coverage.failed > 0 ? (
                <Row
                  label="Failed"
                  value={String(coverage.failed)}
                  tone="negative"
                />
              ) : null}
            </Panel>

            {state.status === 'running' ? (
              <Panel title="Analysing">
                <Text style={styles.nowTitle} numberOfLines={1}>
                  {state.title}
                </Text>
                <View style={styles.progressTrack}>
                  <View
                    style={[
                      styles.progressFill,
                      { width: `${Math.round(state.progress * 100)}%` },
                    ]}
                  />
                </View>
                <Text style={styles.muted}>
                  {Math.round(state.progress * 100)}% decoded
                </Text>
              </Panel>
            ) : null}

            {state.status === 'paused' ? (
              <Panel title="Paused">
                <Text style={styles.warning}>{state.reason}</Text>
              </Panel>
            ) : null}

            {state.status === 'complete' ? (
              <Panel title="Done">
                <Text style={styles.muted}>
                  Every track that can be analysed has been.
                </Text>
              </Panel>
            ) : null}

            {state.status === 'running' ? (
              <Button label="Stop" onPress={stop} variant="secondary" />
            ) : (
              <Button
                label={
                  remaining > 0
                    ? `Analyse ${remaining} track${remaining === 1 ? '' : 's'}`
                    : 'Re-check'
                }
                onPress={() => void start()}
              />
            )}

            <Text style={styles.note}>
              Analysis runs one track at a time and stops if your phone gets
              warm or the battery is low. It pauses when you leave the app -
              there is no background service yet.
            </Text>

            {rows.length > 0 ? (
              <Text style={styles.sectionHeading}>Results</Text>
            ) : null}
          </View>
        }
        renderItem={({ item }) => (
          <View style={styles.card}>
            <Text style={styles.cardTitle} numberOfLines={1}>
              {item.track.title}
            </Text>
            <Row
              label="Tempo"
              value={`${item.analysis.bpm.toFixed(1)} BPM`}
              tone={confidenceTone(item.analysis.bpmConfidence)}
            />
            <Row
              label="Tempo confidence"
              value={item.analysis.bpmConfidence.toFixed(2)}
              tone={confidenceTone(item.analysis.bpmConfidence)}
            />
            {item.analysis.alternateBpm > 0 ? (
              <Row
                label="Could also be"
                value={`${item.analysis.alternateBpm.toFixed(1)} BPM`}
              />
            ) : null}
            <Row
              label="Key"
              value={describeKey(item.analysis.keyTonic, item.analysis.keyMode)}
              tone={confidenceTone(item.analysis.keyConfidence)}
            />
            <Row label="Beats" value={String(item.analysis.beatCount)} />
            <Row
              label="Loudness"
              value={`${item.analysis.integratedLufs.toFixed(1)} LUFS`}
            />
            <Row label="Energy" value={item.analysis.energy.toFixed(2)} />
            <Row label="Sections" value={String(item.analysis.sections.length)} />
            <Row
              label="Music starts"
              value={formatDuration(item.analysis.introEndMs)}
            />
          </View>
        )}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: theme.colour.background },
  list: { padding: theme.space.md },
  nowTitle: {
    color: theme.colour.text,
    fontSize: theme.type.body.fontSize,
    marginBottom: theme.space.sm,
  },
  progressTrack: {
    height: 6,
    borderRadius: 3,
    backgroundColor: theme.colour.surfaceRaised,
    overflow: 'hidden',
  },
  progressFill: { height: 6, backgroundColor: theme.colour.accent },
  muted: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    marginTop: theme.space.xs,
  },
  warning: {
    color: theme.colour.warning,
    fontSize: theme.type.body.fontSize,
    lineHeight: 20,
  },
  note: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    lineHeight: 18,
    marginTop: theme.space.sm,
    marginBottom: theme.space.md,
  },
  sectionHeading: {
    color: theme.colour.text,
    fontSize: theme.type.title.fontSize,
    fontWeight: '600',
    marginBottom: theme.space.sm,
  },
  card: {
    backgroundColor: theme.colour.surface,
    borderColor: theme.colour.border,
    borderWidth: StyleSheet.hairlineWidth,
    borderRadius: theme.radius.md,
    padding: theme.space.md,
    marginBottom: theme.space.sm,
  },
  cardTitle: {
    color: theme.colour.text,
    fontSize: theme.type.body.fontSize,
    fontWeight: '600',
    marginBottom: theme.space.xs,
  },
});
