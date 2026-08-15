/**
 * A single dark theme. The product is a player that people look at in dim
 * rooms and at arm's length, so contrast and type size matter more than
 * offering a light mode nobody will use in this context.
 */
export const theme = {
  colour: {
    background: '#0A0A0F',
    surface: '#14141C',
    surfaceRaised: '#1D1D28',
    border: '#2A2A38',
    text: '#F2F2F7',
    textMuted: '#9A9AAE',
    accent: '#7C5CFF',
    accentMuted: '#3D2E80',
    positive: '#3DDC97',
    warning: '#FFB454',
    negative: '#FF5C6C',
  },
  space: {
    xs: 4,
    sm: 8,
    md: 16,
    lg: 24,
    xl: 32,
  },
  radius: {
    sm: 8,
    md: 12,
    lg: 20,
  },
  type: {
    display: { fontSize: 28, fontWeight: '700' },
    title: { fontSize: 20, fontWeight: '600' },
    body: { fontSize: 15, fontWeight: '400' },
    label: { fontSize: 13, fontWeight: '500' },
    mono: { fontSize: 12, fontFamily: 'monospace' },
  },
} as const;
