import { useCallback, useState } from 'react';
import { useFocusEffect, useRouter } from 'expo-router';
import {
  Alert,
  FlatList,
  Pressable,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';

import type { Playlist } from '@ai-dj/core';

import { Button } from '@/components/Button';
import { createPlaylist, deletePlaylist, listPlaylists } from '@/db/playlists';
import { theme } from '@/theme';

export default function PlaylistsScreen() {
  const router = useRouter();
  const [playlists, setPlaylists] = useState<Playlist[]>([]);
  const [name, setName] = useState('');

  const load = useCallback(async () => {
    setPlaylists(await listPlaylists());
  }, []);

  // Reload on focus so returning from the detail screen shows updated counts.
  useFocusEffect(
    useCallback(() => {
      void load();
    }, [load]),
  );

  const create = useCallback(async () => {
    const trimmed = name.trim();
    if (trimmed.length === 0) return;
    const id = await createPlaylist(trimmed);
    setName('');
    router.push({ pathname: '/playlist/[id]', params: { id: String(id) } });
  }, [name, router]);

  const confirmDelete = useCallback(
    (playlist: Playlist) => {
      Alert.alert(
        'Delete playlist',
        `Delete "${playlist.name}"? Your music files are not affected.`,
        [
          { text: 'Cancel', style: 'cancel' },
          {
            text: 'Delete',
            style: 'destructive',
            onPress: () => {
              void deletePlaylist(playlist.id).then(load);
            },
          },
        ],
      );
    },
    [load],
  );

  return (
    <View style={styles.container}>
      <View style={styles.createRow}>
        <TextInput
          value={name}
          onChangeText={setName}
          placeholder="New playlist name"
          placeholderTextColor={theme.colour.textMuted}
          style={styles.input}
          onSubmitEditing={() => void create()}
          returnKeyType="done"
        />
      </View>
      <View style={styles.createButton}>
        <Button
          label="Create playlist"
          onPress={() => void create()}
          disabled={name.trim().length === 0}
        />
      </View>

      <FlatList
        data={playlists}
        keyExtractor={(playlist) => String(playlist.id)}
        contentContainerStyle={styles.list}
        ListEmptyComponent={
          <Text style={styles.muted}>
            No playlists yet. Create one, then add tracks from your library.
          </Text>
        }
        renderItem={({ item }) => (
          <Pressable
            accessibilityRole="button"
            onPress={() =>
              router.push({
                pathname: '/playlist/[id]',
                params: { id: String(item.id) },
              })
            }
            onLongPress={() => confirmDelete(item)}
            style={({ pressed }) => [styles.row, pressed && styles.rowPressed]}
          >
            <View style={styles.rowText}>
              <Text style={styles.rowTitle}>{item.name}</Text>
              <Text style={styles.rowSubtitle}>
                {item.trackCount} track{item.trackCount === 1 ? '' : 's'}
              </Text>
            </View>
          </Pressable>
        )}
      />

      <Text style={styles.hint}>Long-press a playlist to delete it.</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: theme.colour.background },
  createRow: { paddingHorizontal: theme.space.md, paddingTop: theme.space.md },
  createButton: { paddingHorizontal: theme.space.md },
  input: {
    backgroundColor: theme.colour.surface,
    borderColor: theme.colour.border,
    borderWidth: StyleSheet.hairlineWidth,
    borderRadius: theme.radius.md,
    color: theme.colour.text,
    fontSize: theme.type.body.fontSize,
    marginBottom: theme.space.sm,
    paddingHorizontal: theme.space.md,
    paddingVertical: theme.space.sm,
  },
  list: { padding: theme.space.md, gap: theme.space.sm },
  row: {
    backgroundColor: theme.colour.surface,
    borderColor: theme.colour.border,
    borderWidth: StyleSheet.hairlineWidth,
    borderRadius: theme.radius.md,
    padding: theme.space.md,
  },
  rowPressed: { opacity: 0.75 },
  rowText: { gap: 2 },
  rowTitle: {
    color: theme.colour.text,
    fontSize: theme.type.title.fontSize,
    fontWeight: '600',
  },
  rowSubtitle: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
  },
  muted: {
    color: theme.colour.textMuted,
    fontSize: theme.type.body.fontSize,
    textAlign: 'center',
    padding: theme.space.lg,
  },
  hint: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    textAlign: 'center',
    padding: theme.space.md,
  },
});
