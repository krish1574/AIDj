import { v } from 'convex/values';
import { query, mutation } from './_generated/server';
import { requireUserId } from './lib/identity';
import { DEFAULT_PREFERENCES } from '@ai-dj/core';

const mixStyle = v.union(
  v.literal('smooth'),
  v.literal('balanced'),
  v.literal('energetic'),
);
const transitionLength = v.union(
  v.literal('short'),
  v.literal('medium'),
  v.literal('long'),
);

/**
 * Reactive: the settings screen re-renders itself when preferences change on
 * another device. Returns defaults rather than null for a new user so the
 * client never has to branch on "not created yet".
 */
export const get = query({
  args: {},
  handler: async (ctx) => {
    const userId = await requireUserId(ctx);
    const row = await ctx.db
      .query('userPreferences')
      .withIndex('by_user', (q) => q.eq('userId', userId))
      .unique();

    if (row === null) return DEFAULT_PREFERENCES;

    return {
      mixStyle: row.mixStyle,
      transitionLength: row.transitionLength,
      allowReordering: row.allowReordering,
      targetLufs: row.targetLufs,
    };
  },
});

export const set = mutation({
  args: {
    mixStyle: v.optional(mixStyle),
    transitionLength: v.optional(transitionLength),
    allowReordering: v.optional(v.boolean()),
    targetLufs: v.optional(v.number()),
  },
  handler: async (ctx, args) => {
    const userId = await requireUserId(ctx);

    if (args.targetLufs !== undefined) {
      // Guard rail: a target outside this band either destroys headroom or
      // makes the mix inaudibly quiet relative to everything else on the phone.
      if (args.targetLufs < -24 || args.targetLufs > -8) {
        throw new Error('targetLufs must be between -24 and -8');
      }
    }

    const existing = await ctx.db
      .query('userPreferences')
      .withIndex('by_user', (q) => q.eq('userId', userId))
      .unique();

    if (existing === null) {
      await ctx.db.insert('userPreferences', {
        userId,
        ...DEFAULT_PREFERENCES,
        ...stripUndefined(args),
      });
      return;
    }

    await ctx.db.patch(existing._id, stripUndefined(args));
  },
});

function stripUndefined<T extends object>(o: T): Partial<T> {
  return Object.fromEntries(
    Object.entries(o).filter(([, val]) => val !== undefined),
  ) as Partial<T>;
}
