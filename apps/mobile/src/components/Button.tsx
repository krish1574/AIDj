import { Pressable, StyleSheet, Text } from 'react-native';

import { theme } from '@/theme';

export function Button({
  label,
  onPress,
  disabled = false,
  variant = 'primary',
}: {
  label: string;
  onPress: () => void;
  disabled?: boolean;
  variant?: 'primary' | 'secondary';
}) {
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityState={{ disabled }}
      onPress={onPress}
      disabled={disabled}
      style={({ pressed }) => [
        styles.base,
        variant === 'primary' ? styles.primary : styles.secondary,
        pressed && styles.pressed,
        disabled && styles.disabled,
      ]}
    >
      <Text style={[styles.label, disabled && styles.labelDisabled]}>
        {label}
      </Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  base: {
    paddingVertical: theme.space.md,
    paddingHorizontal: theme.space.lg,
    borderRadius: theme.radius.md,
    alignItems: 'center',
    marginBottom: theme.space.sm,
  },
  primary: { backgroundColor: theme.colour.accent },
  secondary: {
    backgroundColor: theme.colour.surfaceRaised,
    borderColor: theme.colour.border,
    borderWidth: StyleSheet.hairlineWidth,
  },
  pressed: { opacity: 0.75 },
  disabled: { backgroundColor: theme.colour.surfaceRaised, opacity: 0.5 },
  label: {
    color: theme.colour.text,
    fontSize: theme.type.body.fontSize,
    fontWeight: '600',
  },
  labelDisabled: { color: theme.colour.textMuted },
});
