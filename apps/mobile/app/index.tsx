import { useCallback, useEffect, useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import { useRouter } from 'expo-router';

import { Button } from '@/components/Button';
import { Panel, Row } from '@/components/Panel';
import { api, ApiUnavailableError } from '@/api';
import { config, describeMissingConfig } from '@/config';
import { theme } from '@/theme';

type Probe =
  | { status: 'idle' }
  | { status: 'checking' }
  | { status: 'ok'; detail: string; convexReachable: boolean; convexMs: number | null }
  | { status: 'failed'; detail: string };

export default function HomeScreen() {
  const router = useRouter();
  const [probe, setProbe] = useState<Probe>({ status: 'idle' });
  const missingConfig = describeMissingConfig();

  const checkConnectivity = useCallback(async () => {
    setProbe({ status: 'checking' });
    try {
      const response = await api.health();
      if (!response.success) {
        setProbe({ status: 'failed', detail: response.error.message });
        return;
      }
      setProbe({
        status: 'ok',
        detail: `${response.data.service} ${response.data.version}`,
        convexReachable: response.data.convex.reachable,
        convexMs: response.data.convex.latencyMs,
      });
    } catch (error) {
      setProbe({
        status: 'failed',
        detail:
          error instanceof ApiUnavailableError
            ? error.message
            : 'Unexpected error contacting the API.',
      });
    }
  }, []);

  useEffect(() => {
    void checkConnectivity();
  }, [checkConnectivity]);

  return (
    <ScrollView contentContainerStyle={styles.content}>
      <Text style={styles.heading}>AI DJ</Text>
      <Text style={styles.subheading}>
        Milestone 2 - your music library and playlists. Analysis and DJ mixing
        are not built yet: the app can list, organise and order tracks, but it
        cannot yet hear them.
      </Text>

      <Panel title="Backend connectivity">
        <Row label="API base URL" value={config.apiBaseUrl ?? 'not configured'} />
        <Row
          label="API"
          value={
            probe.status === 'checking'
              ? 'checking...'
              : probe.status === 'ok'
                ? probe.detail
                : probe.status === 'failed'
                  ? 'unreachable'
                  : '-'
          }
          tone={
            probe.status === 'ok'
              ? 'positive'
              : probe.status === 'failed'
                ? 'negative'
                : 'default'
          }
        />
        {probe.status === 'ok' ? (
          <Row
            label="API to Convex"
            value={
              probe.convexReachable
                ? `reachable${probe.convexMs !== null ? ` (${probe.convexMs} ms)` : ''}`
                : 'unreachable'
            }
            tone={probe.convexReachable ? 'positive' : 'negative'}
          />
        ) : null}
        {probe.status === 'failed' ? (
          <Text style={styles.error}>{probe.detail}</Text>
        ) : null}
      </Panel>

      {missingConfig.length > 0 ? (
        <Panel title="Configuration needed">
          {missingConfig.map((line) => (
            <Text key={line} style={styles.warning}>
              {line}
            </Text>
          ))}
        </Panel>
      ) : null}

      <Button label="Start AI DJ" onPress={() => router.push('/session')} />
      <Button
        label="Playlists"
        onPress={() => router.push('/playlists')}
        variant="secondary"
      />
      <Button
        label="Music library"
        onPress={() => router.push('/library')}
        variant="secondary"
      />
      <Button
        label="Analysis"
        onPress={() => router.push('/analysis')}
        variant="secondary"
      />
      <Button
        label="Mix test"
        onPress={() => router.push('/mix')}
        variant="secondary"
      />
      <Button label="Re-check connectivity" onPress={() => void checkConnectivity()} variant="secondary" />
      <Button label="Open engine debug" onPress={() => router.push('/debug')} variant="secondary" />

      <View style={styles.notice}>
        <Text style={styles.noticeText}>
          There is no Start AI DJ button yet, and there should not be. Queue
          planning needs track analysis, and mixing needs beat grids - those are
          Milestones 3 to 6. A button that crossfaded two songs on a timer would
          look like the product without being it.
        </Text>
      </View>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  content: { padding: theme.space.md, paddingBottom: theme.space.xl },
  heading: {
    color: theme.colour.text,
    fontSize: theme.type.display.fontSize,
    fontWeight: '700',
    marginBottom: theme.space.xs,
  },
  subheading: {
    color: theme.colour.textMuted,
    fontSize: theme.type.body.fontSize,
    lineHeight: 21,
    marginBottom: theme.space.lg,
  },
  error: {
    color: theme.colour.negative,
    fontSize: theme.type.label.fontSize,
    marginTop: theme.space.sm,
  },
  warning: {
    color: theme.colour.warning,
    fontSize: theme.type.label.fontSize,
    marginBottom: theme.space.xs,
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
