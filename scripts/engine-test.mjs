#!/usr/bin/env node
/**
 * Configures, builds and runs the C++ engine test suite on the desktop host.
 *
 * The engine is deliberately buildable without Android: if verifying a beat
 * tracker ever needs a phone, iteration slows to a crawl and the DSP stops
 * being properly tested. This script is the convenient entry point to that.
 */
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const engineDir = join(repoRoot, 'packages', 'engine');
const buildDir = join(engineDir, 'build');

const cmake = process.env.CMAKE ?? 'cmake';

function run(command, args) {
  const result = spawnSync(command, args, { stdio: 'inherit', shell: false });
  if (result.error !== undefined) {
    console.error(`\nCould not run ${command}: ${result.error.message}`);
    console.error('Set CMAKE to an explicit cmake path if it is not on PATH.');
    process.exit(1);
  }
  if (result.status !== 0) process.exit(result.status ?? 1);
}

const configure = ['-S', engineDir, '-B', buildDir, '-DAIDJ_BUILD_TESTS=ON'];
if (process.env.CMAKE_GENERATOR_OVERRIDE !== undefined) {
  configure.push('-G', process.env.CMAKE_GENERATOR_OVERRIDE);
}

run(cmake, configure);
run(cmake, ['--build', buildDir, '--parallel']);

const candidates = [
  join(buildDir, 'tests', 'aidj_engine_tests'),
  join(buildDir, 'tests', 'aidj_engine_tests.exe'),
];
const binary = candidates.find((path) => existsSync(path));

if (binary === undefined) {
  console.error('Test binary was not produced. See the build output above.');
  process.exit(1);
}

run(binary, []);
