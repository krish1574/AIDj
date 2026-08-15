import { createRemoteJWKSet, jwtVerify } from 'jose';
import type { FastifyRequest } from 'fastify';
import type { Env } from './env.js';

/**
 * Convex Auth is the single issuer of identity. This service does not mint
 * tokens and does not keep a user table - it verifies the deployment's JWT
 * against the deployment's published JWKS and reads the subject.
 *
 * Duplicating an identity model here would be the classic two-sources-of-truth
 * mistake, so the only thing Node knows about a user is their Convex subject.
 */
export interface AuthenticatedUser {
  userId: string;
  email: string | null;
}

export class AuthError extends Error {}

export function createVerifier(env: Env) {
  const jwks = createRemoteJWKSet(
    new URL('/.well-known/jwks.json', env.CONVEX_SITE_URL),
  );

  return async function verify(request: FastifyRequest): Promise<AuthenticatedUser> {
    const header = request.headers.authorization;
    if (typeof header !== 'string' || !header.startsWith('Bearer ')) {
      throw new AuthError('Missing bearer token');
    }
    const token = header.slice('Bearer '.length).trim();
    if (token.length === 0) throw new AuthError('Empty bearer token');

    let payload;
    try {
      ({ payload } = await jwtVerify(token, jwks, {
        issuer: env.CONVEX_SITE_URL,
      }));
    } catch (cause) {
      // Deliberately opaque: signature failure, expiry and wrong issuer are
      // all "not signed in" to the caller. The detail is logged, not returned.
      throw new AuthError('Token verification failed', { cause });
    }

    if (typeof payload.sub !== 'string' || payload.sub.length === 0) {
      throw new AuthError('Token has no subject');
    }

    return {
      userId: payload.sub,
      email: typeof payload['email'] === 'string' ? payload['email'] : null,
    };
  };
}
