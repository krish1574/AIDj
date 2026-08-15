import type { ReactNode } from 'react';
import { StyleSheet, Text, View } from 'react-native';

import { theme } from '@/theme';

export function Panel({
  title,
  children,
}: {
  title: string;
  children: ReactNode;
}) {
  return (
    <View style={styles.panel}>
      <Text style={styles.title}>{title}</Text>
      {children}
    </View>
  );
}

export function Row({ label, value, tone = 'default' }: {
  label: string;
  value: string;
  tone?: 'default' | 'positive' | 'negative' | 'warning';
}) {
  const colour =
    tone === 'positive'
      ? theme.colour.positive
      : tone === 'negative'
        ? theme.colour.negative
        : tone === 'warning'
          ? theme.colour.warning
          : theme.colour.text;

  return (
    <View style={styles.row}>
      <Text style={styles.label}>{label}</Text>
      <Text style={[styles.value, { color: colour }]} numberOfLines={1}>
        {value}
      </Text>
    </View>
  );
}

const styles = StyleSheet.create({
  panel: {
    backgroundColor: theme.colour.surface,
    borderColor: theme.colour.border,
    borderWidth: StyleSheet.hairlineWidth,
    borderRadius: theme.radius.md,
    padding: theme.space.md,
    marginBottom: theme.space.md,
  },
  title: {
    color: theme.colour.textMuted,
    fontSize: theme.type.label.fontSize,
    fontWeight: '600',
    letterSpacing: 1,
    textTransform: 'uppercase',
    marginBottom: theme.space.sm,
  },
  row: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    paddingVertical: theme.space.xs,
    gap: theme.space.md,
  },
  label: {
    color: theme.colour.textMuted,
    fontSize: theme.type.body.fontSize,
  },
  value: {
    fontSize: theme.type.body.fontSize,
    fontFamily: 'monospace',
    flexShrink: 1,
    textAlign: 'right',
  },
});
