import Constants from 'expo-constants';

/**
 * Runtime configuration.
 *
 * A physical device cannot reach `localhost` - that is the device itself. In
 * development we derive the dev machine's LAN address from the Metro host that
 * is already serving this bundle, which removes the single most common
 * "why can't the app reach my API" setup failure.
 */
function inferDevHost(): string | null {
  const hostUri =
    Constants.expoConfig?.hostUri ??
    (Constants.expoGoConfig as { debuggerHost?: string } | undefined)
      ?.debuggerHost;
  if (typeof hostUri !== 'string') return null;
  const host = hostUri.split(':')[0];
  return host !== undefined && host.length > 0 ? host : null;
}

const devHost = inferDevHost();

export const config = {
  apiBaseUrl:
    process.env['EXPO_PUBLIC_API_URL'] ??
    (devHost !== null ? `http://${devHost}:3000` : null),
  convexUrl: process.env['EXPO_PUBLIC_CONVEX_URL'] ?? null,
  isDevelopment: __DEV__,
} as const;

export function describeMissingConfig(): string[] {
  const missing: string[] = [];
  if (config.apiBaseUrl === null) {
    missing.push('EXPO_PUBLIC_API_URL is not set and no dev host was detected');
  }
  if (config.convexUrl === null) {
    missing.push('EXPO_PUBLIC_CONVEX_URL is not set');
  }
  return missing;
}
