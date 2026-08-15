import type { ApiResponse, HealthPayload, WhoAmIPayload } from '@ai-dj/core';
import { config } from './config';

export class ApiUnavailableError extends Error {}

async function request<T>(
  path: string,
  init: RequestInit = {},
): Promise<ApiResponse<T>> {
  if (config.apiBaseUrl === null) {
    throw new ApiUnavailableError('No API base URL is configured.');
  }

  // Without a timeout a request to an unreachable LAN address hangs until the
  // OS gives up, which reads as "the app is frozen".
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 8000);

  try {
    const response = await fetch(`${config.apiBaseUrl}${path}`, {
      ...init,
      signal: controller.signal,
      headers: { Accept: 'application/json', ...init.headers },
    });
    return (await response.json()) as ApiResponse<T>;
  } catch (cause) {
    throw new ApiUnavailableError(
      `Could not reach the API at ${config.apiBaseUrl}.`,
      { cause },
    );
  } finally {
    clearTimeout(timeout);
  }
}

export const api = {
  health: () => request<HealthPayload>('/api/v1/health'),
  me: (token: string) =>
    request<WhoAmIPayload>('/api/v1/me', {
      headers: { Authorization: `Bearer ${token}` },
    }),
};
