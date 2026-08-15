import { convexAuth } from '@convex-dev/auth/server';
import Resend from '@auth/core/providers/resend';
import { Anonymous } from '@convex-dev/auth/providers/Anonymous';

/**
 * Email one-time-code sign-in.
 *
 * Chosen over password for V1: no password storage, no reset flow, and it is
 * the least friction on mobile. OAuth providers can be added to this array
 * later without touching the identity model, because Convex Auth keys users on
 * the `users` table either way. Requires AUTH_RESEND_KEY in the Convex
 * deployment environment.
 *
 * The Anonymous provider is enabled only when AIDJ_ALLOW_ANONYMOUS_AUTH is set
 * on the deployment. It exists so the app can be exercised end-to-end before
 * an email sender is configured. It must never be set on a production
 * deployment - it lets anyone mint a session.
 */
const allowAnonymous = process.env.AIDJ_ALLOW_ANONYMOUS_AUTH === 'true';

export const { auth, signIn, signOut, store, isAuthenticated } = convexAuth({
  providers: allowAnonymous
    ? [Resend({ apiKey: process.env.AUTH_RESEND_KEY }), Anonymous]
    : [Resend({ apiKey: process.env.AUTH_RESEND_KEY })],
});
