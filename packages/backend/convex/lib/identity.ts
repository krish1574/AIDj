import type { QueryCtx, MutationCtx } from '../_generated/server';
import type { Id } from '../_generated/dataModel';
import { getAuthUserId } from '@convex-dev/auth/server';

/**
 * The only sanctioned way to learn who is calling.
 *
 * Every user-scoped query and mutation goes through this. Nothing accepts a
 * user id as an argument - if a function signature ever grows a `userId`
 * parameter, that is a bug, not a shortcut.
 */
export async function requireUserId(
  ctx: QueryCtx | MutationCtx,
): Promise<Id<'users'>> {
  const userId = await getAuthUserId(ctx);
  if (userId === null) {
    throw new Error('UNAUTHORISED');
  }
  return userId;
}
