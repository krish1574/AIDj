import { Image, Pressable, StyleSheet, Text, View } from 'react-native';

import { formatDuration, type LibraryTrack } from '@ai-dj/core';

import { theme } from '@/theme';

/**
 * One row in a library or playlist list.
 *
 * Artwork is rendered straight from the MediaStore content:// URI rather than
 * copied into app storage - the grant is already held, and duplicating album
 * art for a large library would waste real disk.
 */
export function TrackRow({
  track,
  selected = false,
  index,
  onPress,
  onLongPress,
  trailing,
}: {
  track: LibraryTrack;
  selected?: boolean;
  index?: number;
  onPress?: () => void;
  onLongPress?: () => void;
  trailing?: React.ReactNode;
}) {
  return (
    <Pressable
      onPress={onPress}
      onLongPress={onLongPress}
      accessibilityRole={onPress ? 'button' : undefined}
      accessibilityState={onPress ? { selected } : undefined}
      style={({ pressed }) => [
        styles.row,
        selected && styles.rowSelected,
        pressed && onPress && styles.rowPressed,
      ]}
    >
      {index !== undefined ? (
        <Text style={styles.index}>{index + 1}</Text>
      ) : null}

      {track.artworkUri ? (
        <Image
          source={{ uri: track.artworkUri }}
          style={styles.artwork}
          // Album art is frequently missing even when MediaStore advertises a
          // URI; a failed load must not break the row.
          onError={() => undefined}
        />
      ) : (
        <View style={[styles.artwork, styles.artworkEmpty]} />
      )}

      <View style={styles.text}>
        <Text style={styles.title} numberOfLines={1}>
          {track.title}
        </Text>
        <Text style={styles.subtitle} numberOfLines={1}>
          {track.artist ?? 'Unknown artist'}
        </Text>
      </View>

      <Text style={styles.duration}>{formatDuration(track.durationMs)}</Text>
      {trailing}
    </Pressable>
  );
}

const styles = StyleSheet.create({
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    paddingVertical: theme.space.sm,
    paddingHorizontal: theme.space.md,
    gap: theme.space.md,
    borderRadius: theme.radius.sm,
  },
  rowSelected: { backgroundColor: theme.colour.accentMuted },
  rowPressed: { opacity: 0.7 },
  index: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    fontFamily: 'monospace',
    minWidth: 24,
  },
  artwork: {
    width: 44,
    height: 44,
    borderRadius: theme.radius.sm,
    backgroundColor: theme.colour.surfaceRaised,
  },
  artworkEmpty: { borderColor: theme.colour.border, borderWidth: StyleSheet.hairlineWidth },
  text: { flex: 1, gap: 2 },
  title: {
    color: theme.colour.text,
    fontSize: theme.type.body.fontSize,
    fontWeight: '500',
  },
  subtitle: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
  },
  duration: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    fontFamily: 'monospace',
  },
});
