import { query } from './_generated/server';
import { getAuthUserId } from '@convex-dev/auth/server';

/**
 * Connectivity probe. Used by the Fastify health endpoint and by the mobile
 * debug screen to prove a real round-trip rather than a cached client state.
 * Deliberately does no database work beyond the auth lookup.
 */
export const ping = query({
  args: {},
  handler: async (ctx) => {
    const userId = await getAuthUserId(ctx);
    return {
      ok: true as const,
      serverTimeMs: Date.now(),
      authenticated: userId !== null,
    };
  },
});
