import { describe, it, expect } from 'vitest';
import { buildApp } from './app.js';
import { loadEnv } from './env.js';
import type { Env } from './env.js';

const testEnv: Env = {
  NODE_ENV: 'test',
  PORT: 0,
  HOST: '127.0.0.1',
  // Unreachable on purpose: health must degrade, not throw, when Convex is down.
  CONVEX_URL: 'https://unreachable.convex.cloud',
  CONVEX_SITE_URL: 'https://unreachable.convex.site',
};

describe('env', () => {
  it('rejects a missing CONVEX_URL rather than starting', () => {
    expect(() => loadEnv({ CONVEX_SITE_URL: 'https://x.convex.site' })).toThrow(
      /CONVEX_URL/,
    );
  });

  it('rejects a non-URL CONVEX_URL', () => {
    expect(() =>
      loadEnv({ CONVEX_URL: 'not-a-url', CONVEX_SITE_URL: 'https://x.convex.site' }),
    ).toThrow(/CONVEX_URL/);
  });
});

describe('GET /api/v1/health', () => {
  it('reports unreachable Convex without failing the request', async () => {
    const app = await buildApp({ env: testEnv });
    const res = await app.inject({ method: 'GET', url: '/api/v1/health' });

    expect(res.statusCode).toBe(200);
    const body = res.json();
    expect(body.success).toBe(true);
    expect(body.data.service).toBe('ai-dj-api');
    expect(body.data.convex.reachable).toBe(false);
    expect(body.data.convex.latencyMs).toBeNull();
    await app.close();
  });
});

describe('GET /api/v1/me', () => {
  it('rejects a request with no token', async () => {
    const app = await buildApp({ env: testEnv });
    const res = await app.inject({ method: 'GET', url: '/api/v1/me' });

    expect(res.statusCode).toBe(401);
    expect(res.json()).toEqual({
      success: false,
      error: { code: 'UNAUTHORISED', message: 'Please sign in again.' },
    });
    await app.close();
  });

  it('rejects a malformed token without leaking why', async () => {
    const app = await buildApp({ env: testEnv });
    const res = await app.inject({
      method: 'GET',
      url: '/api/v1/me',
      headers: { authorization: 'Bearer not.a.jwt' },
    });

    expect(res.statusCode).toBe(401);
    expect(JSON.stringify(res.json())).not.toMatch(/jose|signature|jwks/i);
    await app.close();
  });
});

describe('unknown routes', () => {
  it('returns the standard failure envelope', async () => {
    const app = await buildApp({ env: testEnv });
    const res = await app.inject({ method: 'GET', url: '/api/v1/nope' });

    expect(res.statusCode).toBe(404);
    expect(res.json().error.code).toBe('NOT_FOUND');
    await app.close();
  });
});
