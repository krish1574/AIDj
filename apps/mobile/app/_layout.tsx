import { ConvexProvider, ConvexReactClient } from 'convex/react';
import { Stack } from 'expo-router';
import { StatusBar } from 'expo-status-bar';
import { SafeAreaProvider } from 'react-native-safe-area-context';

import { config } from '@/config';
import { theme } from '@/theme';

/**
 * One Convex client for the process. Creating it inside a component would tear
 * down and rebuild the websocket on every re-render.
 *
 * When no Convex URL is configured the app still runs - the offline-first
 * posture means a missing backend degrades the account features, not the app.
 */
const convex =
  config.convexUrl !== null
    ? new ConvexReactClient(config.convexUrl, { unsavedChangesWarning: false })
    : null;

export default function RootLayout() {
  const content = (
    <>
      <StatusBar style="light" />
      <Stack
        screenOptions={{
          headerStyle: { backgroundColor: theme.colour.background },
          headerTintColor: theme.colour.text,
          headerShadowVisible: false,
          contentStyle: { backgroundColor: theme.colour.background },
        }}
      >
        <Stack.Screen name="index" options={{ title: 'AI DJ' }} />
        <Stack.Screen name="library" options={{ title: 'Library' }} />
        <Stack.Screen name="playlists" options={{ title: 'Playlists' }} />
        <Stack.Screen name="playlist/[id]" options={{ title: 'Playlist' }} />
        <Stack.Screen name="debug" options={{ title: 'Engine Debug' }} />
      </Stack>
    </>
  );

  return (
    <SafeAreaProvider>
      {convex !== null ? (
        <ConvexProvider client={convex}>{content}</ConvexProvider>
      ) : (
        content
      )}
    </SafeAreaProvider>
  );
}
