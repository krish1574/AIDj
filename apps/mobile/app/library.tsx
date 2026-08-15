import { useCallback, useState } from 'react';
import {
  ActivityIndicator,
  FlatList,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import { useLocalSearchParams, useRouter } from 'expo-router';

import type { LibraryTrack } from '@ai-dj/core';

import { Button } from '@/components/Button';
import { TrackRow } from '@/components/TrackRow';
import { useLibrary } from '@/features/library/useLibrary';
import { addTracks } from '@/db/playlists';
import { theme } from '@/theme';

/**
 * The device's music, and the picker for adding to a playlist.
 *
 * Doubles as a picker when opened with a `playlistId` param: rows become
 * multi-select and a confirm button appears. One screen rather than two
 * because the list, search and empty states are identical either way.
 */
export default function LibraryScreen() {
  const router = useRouter();
  const params = useLocalSearchParams<{ playlistId?: string }>();
  const playlistId = params.playlistId ? Number(params.playlistId) : null;
  const isPicker = playlistId !== null && Number.isFinite(playlistId);

  const { state, query, setQuery, scan } = useLibrary();
  const [selected, setSelected] = useState<Set<number>>(new Set());
  const [saving, setSaving] = useState(false);

  const toggle = useCallback((trackId: number) => {
    setSelected((current) => {
      const next = new Set(current);
      if (next.has(trackId)) next.delete(trackId);
      else next.add(trackId);
      return next;
    });
  }, []);

  const confirm = useCallback(async () => {
    if (!isPicker || selected.size === 0) return;
    setSaving(true);
    try {
      await addTracks(playlistId, [...selected]);
      router.back();
    } finally {
      setSaving(false);
    }
  }, [isPicker, playlistId, router, selected]);

  const renderTrack = useCallback(
    ({ item }: { item: LibraryTrack }) => (
      <TrackRow
        track={item}
        selected={selected.has(item.id)}
        // Spread rather than pass undefined: exactOptionalPropertyTypes makes
        // "absent" and "present but undefined" different types.
        {...(isPicker ? { onPress: () => toggle(item.id) } : {})}
      />
    ),
    [isPicker, selected, toggle],
  );

  if (state.status === 'scanning') {
    return (
      <View style={styles.centred}>
        <ActivityIndicator color={theme.colour.accent} />
        <Text style={styles.muted}>Scanning your music…</Text>
      </View>
    );
  }

  if (state.status === 'needsPermission') {
    return (
      <View style={styles.centred}>
        <Text style={styles.message}>
          AI DJ needs permission to read audio files to build your library. It
          only reads - your files are never modified or moved.
        </Text>
        <Button label="Grant permission" onPress={() => void scan()} />
      </View>
    );
  }

  if (state.status === 'failed') {
    return (
      <View style={styles.centred}>
        <Text style={styles.error}>{state.message}</Text>
        <Button label="Try again" onPress={() => void scan()} />
      </View>
    );
  }

  const tracks = state.status === 'ready' ? state.tracks : [];
  const isEmpty = state.status === 'ready' && tracks.length === 0;

  return (
    <View style={styles.container}>
      <TextInput
        value={query}
        onChangeText={setQuery}
        placeholder="Search title, artist or album"
        placeholderTextColor={theme.colour.textMuted}
        style={styles.search}
        autoCorrect={false}
      />

      {isEmpty ? (
        <View style={styles.centred}>
          <Text style={styles.message}>
            {query.length > 0
              ? 'Nothing matches that search.'
              : 'No music found yet. Scan to build your library.'}
          </Text>
          <Button label="Scan device" onPress={() => void scan()} />
        </View>
      ) : (
        <FlatList
          data={tracks}
          keyExtractor={(track) => String(track.id)}
          renderItem={renderTrack}
          contentContainerStyle={styles.list}
          // Rows are a fixed height, so this avoids measuring every one of
          // them on a large library.
          initialNumToRender={20}
          windowSize={10}
        />
      )}

      <View style={styles.footer}>
        {isPicker ? (
          <Button
            label={
              saving
                ? 'Adding…'
                : `Add ${selected.size} track${selected.size === 1 ? '' : 's'}`
            }
            onPress={() => void confirm()}
            disabled={selected.size === 0 || saving}
          />
        ) : (
          <Button label="Rescan device" variant="secondary" onPress={() => void scan()} />
        )}
        {state.status === 'ready' && state.lastScan ? (
          <Text style={styles.muted}>
            {state.lastScan.total} tracks · {state.lastScan.added} added ·{' '}
            {state.lastScan.removed} removed
          </Text>
        ) : null}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: theme.colour.background },
  centred: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    padding: theme.space.lg,
    gap: theme.space.md,
    backgroundColor: theme.colour.background,
  },
  search: {
    backgroundColor: theme.colour.surface,
    borderColor: theme.colour.border,
    borderWidth: StyleSheet.hairlineWidth,
    borderRadius: theme.radius.md,
    color: theme.colour.text,
    fontSize: theme.type.body.fontSize,
    margin: theme.space.md,
    paddingHorizontal: theme.space.md,
    paddingVertical: theme.space.sm,
  },
  list: { paddingHorizontal: theme.space.sm, paddingBottom: theme.space.md },
  footer: {
    padding: theme.space.md,
    borderTopColor: theme.colour.border,
    borderTopWidth: StyleSheet.hairlineWidth,
  },
  message: {
    color: theme.colour.text,
    fontSize: theme.type.body.fontSize,
    textAlign: 'center',
  },
  muted: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    textAlign: 'center',
  },
  error: {
    color: theme.colour.negative,
    fontSize: theme.type.body.fontSize,
    textAlign: 'center',
  },
});
