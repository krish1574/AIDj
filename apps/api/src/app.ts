import Fastify, { type FastifyInstance } from 'fastify';
import { ConvexHttpClient } from 'convex/browser';
import { makeFunctionReference } from 'convex/server';
import type {
  ApiFailure,
  ApiResponse,
  HealthPayload,
  WhoAmIPayload,
} from '@ai-dj/core';
import { API_VERSION } from '@ai-dj/core';
import { loadEnv, type Env } from './env.js';
import { createVerifier, AuthError } from './auth.js';

/**
 * Referenced by name rather than through the generated `api` object: this
 * service must build in CI without a Convex codegen step, and `system:ping`
 * is a stable contract. Anything with a real argument shape should use the
 * generated types instead.
 */
const pingRef = makeFunctionReference<
  'query',
  Record<string, never>,
  { ok: true; serverTimeMs: number; authenticated: boolean }
>('system:ping');

const SERVICE_NAME = 'ai-dj-api';
const SERVICE_VERSION = '0.1.0';
const CONVEX_PING_TIMEOUT_MS = 2000;

function ok<T>(data: T, message = 'Success'): ApiResponse<T> {
  return { success: true, data, message };
}

function fail(code: ApiFailure['error']['code'], message: string): ApiFailure {
  return { success: false, error: { code, message } };
}

export interface BuildOptions {
  env?: Env;
}

export async function buildApp(options: BuildOptions = {}): Promise<FastifyInstance> {
  const env = options.env ?? loadEnv();
  const app = Fastify({
    logger: env.NODE_ENV !== 'test',
    // The app sits behind nothing in dev but will sit behind a proxy later.
    trustProxy: true,
  });

  const verify = createVerifier(env);
  const convex = new ConvexHttpClient(env.CONVEX_URL);
  const startedAt = Date.now();

  /**
   * Never leak internals. Validation and auth failures get a specific,
   * user-safe code; everything else collapses to INTERNAL and is logged
   * server-side with the real cause.
   */
  app.setErrorHandler((error: unknown, request, reply) => {
    if (error instanceof AuthError) {
      request.log.info({ err: error }, 'auth rejected');
      return reply.code(401).send(fail('UNAUTHORISED', 'Please sign in again.'));
    }
    const fastifyError = error as { statusCode?: number; validation?: unknown };
    if (fastifyError.statusCode === 400 || fastifyError.validation != null) {
      return reply.code(400).send(fail('BAD_REQUEST', 'The request was not valid.'));
    }
    request.log.error({ err: error }, 'unhandled error');
    return reply
      .code(500)
      .send(fail('INTERNAL', 'Something went wrong. Please try again.'));
  });

  app.setNotFoundHandler((_request, reply) =>
    reply.code(404).send(fail('NOT_FOUND', 'That endpoint does not exist.')),
  );

  const prefix = `/api/${API_VERSION}`;

  /**
   * Health does a live Convex round-trip rather than reporting a cached flag,
   * because "the API is up" is not the interesting question - "can the API
   * still reach its database" is.
   */
  app.get(`${prefix}/health`, async (): Promise<ApiResponse<HealthPayload>> => {
    let reachable = false;
    let latencyMs: number | null = null;

    const started = performance.now();
    try {
      // The Convex client retries internally and will happily spend a long
      // time on an unreachable deployment. A health endpoint that hangs is
      // worse than one that reports a failure, so we cap it ourselves.
      await Promise.race([
        convex.query(pingRef, {}),
        new Promise((_resolve, reject) =>
          setTimeout(
            () => reject(new Error('convex ping timed out')),
            CONVEX_PING_TIMEOUT_MS,
          ),
        ),
      ]);
      reachable = true;
      latencyMs = Math.round(performance.now() - started);
    } catch (cause) {
      app.log.warn({ err: cause }, 'convex ping failed');
    }

    return ok({
      service: SERVICE_NAME,
      version: SERVICE_VERSION,
      uptimeSeconds: Math.floor((Date.now() - startedAt) / 1000),
      convex: { reachable, latencyMs },
    });
  });

  /**
   * Proves the full chain: mobile holds a Convex-issued token, Node verifies
   * it against Convex's JWKS, and the identity comes back. If this returns a
   * user id, RN -> Node -> Convex auth is genuinely wired.
   */
  app.get(`${prefix}/me`, async (request): Promise<ApiResponse<WhoAmIPayload>> => {
    const user = await verify(request);
    return ok({ userId: user.userId, email: user.email });
  });

  return app;
}
