import { useCallback, useState } from 'react';
import { useFocusEffect, useLocalSearchParams, useRouter } from 'expo-router';
import { FlatList, Pressable, StyleSheet, Text, View } from 'react-native';

import type { LibraryTrack, Playlist } from '@ai-dj/core';
import { formatDuration } from '@ai-dj/core';

import { Button } from '@/components/Button';
import { TrackRow } from '@/components/TrackRow';
import {
  getPlaylist,
  moveTrack,
  playlistTracks,
  removeTrack,
} from '@/db/playlists';
import { theme } from '@/theme';

export default function PlaylistDetailScreen() {
  const router = useRouter();
  const { id } = useLocalSearchParams<{ id: string }>();
  const playlistId = Number(id);

  const [playlist, setPlaylist] = useState<Playlist | null>(null);
  const [tracks, setTracks] = useState<LibraryTrack[]>([]);

  const load = useCallback(async () => {
    if (!Number.isFinite(playlistId)) return;
    setPlaylist(await getPlaylist(playlistId));
    setTracks(await playlistTracks(playlistId));
  }, [playlistId]);

  useFocusEffect(
    useCallback(() => {
      void load();
    }, [load]),
  );

  const move = useCallback(
    async (trackId: number, toIndex: number) => {
      await moveTrack(playlistId, trackId, toIndex);
      await load();
    },
    [load, playlistId],
  );

  const remove = useCallback(
    async (trackId: number) => {
      await removeTrack(playlistId, trackId);
      await load();
    },
    [load, playlistId],
  );

  const totalMs = tracks.reduce((sum, track) => sum + track.durationMs, 0);

  return (
    <View style={styles.container}>
      <View style={styles.header}>
        <Text style={styles.title}>{playlist?.name ?? 'Playlist'}</Text>
        <Text style={styles.subtitle}>
          {tracks.length} track{tracks.length === 1 ? '' : 's'} ·{' '}
          {formatDuration(totalMs)}
        </Text>
      </View>

      <FlatList
        data={tracks}
        keyExtractor={(track) => String(track.id)}
        contentContainerStyle={styles.list}
        ListEmptyComponent={
          <Text style={styles.muted}>
            This playlist is empty. Add tracks from your library.
          </Text>
        }
        renderItem={({ item, index }) => (
          <TrackRow
            track={item}
            index={index}
            trailing={
              <View style={styles.controls}>
                <Pressable
                  accessibilityLabel="Move up"
                  accessibilityRole="button"
                  disabled={index === 0}
                  onPress={() => void move(item.id, index - 1)}
                  style={styles.control}
                >
                  <Text
                    style={[styles.controlText, index === 0 && styles.controlDisabled]}
                  >
                    ↑
                  </Text>
                </Pressable>
                <Pressable
                  accessibilityLabel="Move down"
                  accessibilityRole="button"
                  disabled={index === tracks.length - 1}
                  onPress={() => void move(item.id, index + 1)}
                  style={styles.control}
                >
                  <Text
                    style={[
                      styles.controlText,
                      index === tracks.length - 1 && styles.controlDisabled,
                    ]}
                  >
                    ↓
                  </Text>
                </Pressable>
                <Pressable
                  accessibilityLabel="Remove from playlist"
                  accessibilityRole="button"
                  onPress={() => void remove(item.id)}
                  style={styles.control}
                >
                  <Text style={[styles.controlText, styles.remove]}>✕</Text>
                </Pressable>
              </View>
            }
          />
        )}
      />

      <View style={styles.footer}>
        <Button
          label="Add tracks"
          onPress={() =>
            router.push({
              pathname: '/library',
              params: { playlistId: String(playlistId) },
            })
          }
        />
        <Text style={styles.note}>
          There is no Start AI DJ button yet. Ordering a playlist is not the
          same as planning a DJ queue - that needs BPM, key and beat grids from
          Milestones 3 and 4.
        </Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: theme.colour.background },
  header: { padding: theme.space.md, gap: theme.space.xs },
  title: {
    color: theme.colour.text,
    fontSize: theme.type.display.fontSize,
    fontWeight: '700',
  },
  subtitle: {
    color: theme.colour.textMuted,
    fontSize: theme.type.body.fontSize,
  },
  list: { paddingHorizontal: theme.space.sm, paddingBottom: theme.space.md },
  controls: { flexDirection: 'row', gap: theme.space.xs },
  control: { padding: theme.space.sm },
  controlText: {
    color: theme.colour.text,
    fontSize: theme.type.title.fontSize,
  },
  controlDisabled: { color: theme.colour.border },
  remove: { color: theme.colour.negative },
  footer: {
    padding: theme.space.md,
    borderTopColor: theme.colour.border,
    borderTopWidth: StyleSheet.hairlineWidth,
    gap: theme.space.sm,
  },
  muted: {
    color: theme.colour.textMuted,
    fontSize: theme.type.body.fontSize,
    textAlign: 'center',
    padding: theme.space.lg,
  },
  note: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    lineHeight: 18,
  },
});
