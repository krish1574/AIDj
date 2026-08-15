import { z } from 'zod';

/**
 * Fail fast and loudly at boot rather than at the first request. A missing
 * CONVEX_URL should stop the process, not produce a 500 an hour later.
 */
const schema = z.object({
  NODE_ENV: z.enum(['development', 'test', 'production']).default('development'),
  PORT: z.coerce.number().int().min(1).max(65535).default(3000),
  HOST: z.string().default('0.0.0.0'),
  /** e.g. https://<deployment>.convex.cloud - used for function calls. */
  CONVEX_URL: z.url(),
  /**
   * e.g. https://<deployment>.convex.site - the HTTP actions origin. JWTs
   * issued by Convex Auth carry this as their issuer and its JWKS signs them.
   */
  CONVEX_SITE_URL: z.url(),
});

export type Env = z.infer<typeof schema>;

export function loadEnv(source: NodeJS.ProcessEnv = process.env): Env {
  const parsed = schema.safeParse(source);
  if (!parsed.success) {
    const detail = parsed.error.issues
      .map((i) => `${i.path.join('.')}: ${i.message}`)
      .join('; ');
    throw new Error(`Invalid environment configuration - ${detail}`);
  }
  return parsed.data;
}
