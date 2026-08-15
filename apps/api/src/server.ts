import { existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { buildApp } from './app.js';
import { loadEnv } from './env.js';

/**
 * `convex dev` owns packages/backend/.env.local and rewrites CONVEX_URL there
 * whenever the deployment changes, so we read it rather than keeping a second
 * copy that goes stale. apps/api/.env is loaded after it and wins, which is how
 * you point the API at a different deployment without editing Convex's file.
 * Real process env still beats both - `process.loadEnvFile` does not overwrite
 * variables that are already set.
 */
const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
for (const file of ['packages/backend/.env.local', 'apps/api/.env']) {
  const path = resolve(repoRoot, file);
  if (existsSync(path)) process.loadEnvFile(path);
}

const env = loadEnv();
const app = await buildApp({ env });

try {
  await app.listen({ port: env.PORT, host: env.HOST });
} catch (error) {
  app.log.error({ err: error }, 'failed to start');
  process.exit(1);
}

for (const signal of ['SIGINT', 'SIGTERM'] as const) {
  process.on(signal, () => {
    void app.close().then(() => process.exit(0));
  });
}
