import { useCallback, useEffect, useRef, useState } from 'react';
import { ScrollView, StyleSheet, Text } from 'react-native';
import * as DocumentPicker from 'expo-document-picker';

import { AiDjAudio } from 'aidj-audio';
import type { EngineStatus, VoiceIndex } from '@ai-dj/core';
import { Button } from '@/components/Button';
import { Panel, Row } from '@/components/Panel';
import { theme } from '@/theme';

/** 4 Hz is enough to watch the engine without perturbing what it measures. */
const POLL_INTERVAL_MS = 250;

export default function DebugScreen() {
  const [status, setStatus] = useState<EngineStatus | null>(null);
  const [log, setLog] = useState<string[]>([]);
  const [loaded, setLoaded] = useState<Record<number, string>>({});
  const initialised = useRef(false);

  const append = useCallback((line: string) => {
    setLog((previous) => [`${new Date().toLocaleTimeString()}  ${line}`, ...previous].slice(0, 12));
  }, []);

  const run = useCallback(
    async (description: string, action: () => Promise<void>) => {
      try {
        await action();
        append(`${description}: ok`);
      } catch (error) {
        const code =
          typeof error === 'object' && error !== null && 'code' in error
            ? String((error as { code: unknown }).code)
            : 'ERROR';
        append(`${description}: ${code}`);
      }
    },
    [append],
  );

  useEffect(() => {
    if (initialised.current) return;
    initialised.current = true;
    void run('initialise', () => AiDjAudio.initialise());
  }, [run]);

  useEffect(() => {
    const timer = setInterval(() => {
      try {
        setStatus(AiDjAudio.getStatus());
      } catch {
        // The module is unavailable in a build without the native module -
        // leave the last status rather than crashing the debug screen.
      }
    }, POLL_INTERVAL_MS);
    return () => clearInterval(timer);
  }, []);

  const pickInto = useCallback(
    async (voice: VoiceIndex) => {
      const result = await DocumentPicker.getDocumentAsync({
        type: 'audio/*',
        copyToCacheDirectory: false,
      });
      const asset = result.assets?.[0];
      if (result.canceled || asset === undefined) return;

      await run(`load voice ${voice}`, () =>
        AiDjAudio.loadVoice(voice, asset.uri),
      );
      setLoaded((previous) => ({ ...previous, [voice]: asset.name }));
    },
    [run],
  );

  const bothLoaded = loaded[0] !== undefined && loaded[1] !== undefined;

  return (
    <ScrollView contentContainerStyle={styles.content}>
      <Panel title="Engine">
        <Row label="State" value={status?.state ?? 'unknown'} />
        <Row
          label="Underruns"
          value={String(status?.underrunCount ?? 0)}
          tone={(status?.underrunCount ?? 0) > 0 ? 'negative' : 'positive'}
        />
        <Row
          label="Starved frames"
          value={String(status?.starvedFrames ?? 0)}
          tone={(status?.starvedFrames ?? 0) > 0 ? 'warning' : 'positive'}
        />
        <Row label="Frames per burst" value={String(status?.framesPerBurst ?? 0)} />
        <Row
          label="Output latency"
          value={
            status?.outputLatencyMs != null
              ? `${status.outputLatencyMs.toFixed(1)} ms`
              : 'not reported'
          }
        />
        <Row
          label="Last error"
          value={status?.lastError ?? 'NONE'}
          tone={status?.lastError && status.lastError !== 'NONE' ? 'negative' : 'default'}
        />
      </Panel>

      {[0, 1].map((index) => {
        const voice = status?.voices?.[index];
        return (
          <Panel key={index} title={`Voice ${index}`}>
            <Row label="File" value={loaded[index] ?? 'none'} />
            <Row label="Primed" value={voice?.primed === true ? 'yes' : 'no'} tone={voice?.primed === true ? 'positive' : 'default'} />
            <Row
              label="Position"
              value={`${((voice?.positionMs ?? 0) / 1000).toFixed(1)} s / ${((voice?.durationMs ?? 0) / 1000).toFixed(1)} s`}
            />
            <Row label="Gain" value={(voice?.gain ?? 0).toFixed(3)} />
            <Row label="End of stream" value={voice?.endOfStream === true ? 'yes' : 'no'} />
            <Button
              label={`Pick audio for voice ${index}`}
              variant="secondary"
              onPress={() => void pickInto(index as VoiceIndex)}
            />
            <Button
              label={`Play voice ${index}`}
              variant="secondary"
              disabled={loaded[index] === undefined}
              onPress={() =>
                void run(`play voice ${index}`, () =>
                  AiDjAudio.playVoice(index as VoiceIndex),
                )
              }
            />
          </Panel>
        );
      })}

      <Panel title="Transport">
        <Button label="Pause" variant="secondary" onPress={() => void run('pause', () => AiDjAudio.pause())} />
        <Button label="Resume" variant="secondary" onPress={() => void run('resume', () => AiDjAudio.resume())} />
        <Button label="Stop all" variant="secondary" onPress={() => void run('stop', () => AiDjAudio.stopAll())} />
      </Panel>

      <Panel title="Developer crossfade">
        <Text style={styles.caution}>
          Fixed-length equal-power gain automation between the two voices. No
          beat alignment, no tempo matching, no EQ. This proves the automation
          path is sample-accurate - it is not the transition engine, which
          needs the beat grids from Milestone 3.
        </Text>
        <Button
          label="Crossfade 0 to 1 over 4 s"
          disabled={!bothLoaded}
          onPress={() => void run('crossfade 0->1', () => AiDjAudio.devCrossfade(0, 1, 4000))}
        />
        <Button
          label="Crossfade 1 to 0 over 4 s"
          disabled={!bothLoaded}
          onPress={() => void run('crossfade 1->0', () => AiDjAudio.devCrossfade(1, 0, 4000))}
        />
      </Panel>

      <Panel title="Log">
        {log.length === 0 ? (
          <Text style={styles.logLine}>No activity yet.</Text>
        ) : (
          log.map((line, index) => (
            <Text key={`${line}-${index}`} style={styles.logLine}>
              {line}
            </Text>
          ))
        )}
      </Panel>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  content: { padding: theme.space.md, paddingBottom: theme.space.xl },
  caution: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    lineHeight: 18,
    marginBottom: theme.space.sm,
  },
  logLine: {
    color: theme.colour.textMuted,
    fontFamily: 'monospace',
    fontSize: theme.type.mono.fontSize,
    marginBottom: 2,
  },
});
