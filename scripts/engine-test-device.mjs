#!/usr/bin/env node
/**
 * Builds the engine test suite for arm64 Android and runs it on an attached
 * device via adb.
 *
 * The host path (scripts/engine-test.mjs) is preferred when a desktop C++
 * toolchain exists, because it iterates faster. This exists because a machine
 * can have a complete Android toolchain and no host compiler at all - and
 * running on-device has a real advantage: the DSP is verified on the exact
 * architecture and floating-point behaviour it will ship on.
 */
import { spawnSync } from 'node:child_process';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const engineDir = join(repoRoot, 'packages', 'engine');
const buildDir = join(engineDir, 'build-android');

const sdk = process.env.ANDROID_HOME ?? process.env.ANDROID_SDK_ROOT;
if (sdk === undefined) {
  console.error('Set ANDROID_HOME (see scripts/android-env.bat).');
  process.exit(1);
}

const ndkVersion = process.env.AIDJ_NDK_VERSION ?? '27.1.12297006';
const cmakeVersion = process.env.AIDJ_CMAKE_VERSION ?? '3.22.1';

const ndk = join(sdk, 'ndk', ndkVersion);
const cmake = join(sdk, 'cmake', cmakeVersion, 'bin', 'cmake.exe');
const ninja = join(sdk, 'cmake', cmakeVersion, 'bin', 'ninja.exe');
const adb = join(sdk, 'platform-tools', 'adb.exe');

/** Where the binary lands on the device. Cleaned up after the run. */
const deviceDir = '/data/local/tmp/aidj-tests';

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: 'inherit', ...options });
  if (result.error !== undefined) {
    console.error(`Could not run ${command}: ${result.error.message}`);
    process.exit(1);
  }
  if (result.status !== 0) process.exit(result.status ?? 1);
  return result;
}

run(cmake, [
  '-S', engineDir,
  '-B', buildDir,
  '-G', 'Ninja',
  `-DCMAKE_MAKE_PROGRAM=${ninja}`,
  `-DCMAKE_TOOLCHAIN_FILE=${join(ndk, 'build', 'cmake', 'android.toolchain.cmake')}`,
  '-DANDROID_ABI=arm64-v8a',
  '-DANDROID_PLATFORM=android-26',
  '-DANDROID_STL=c++_shared',
  '-DCMAKE_BUILD_TYPE=Release',
  '-DAIDJ_BUILD_TESTS=ON',
  // The device runs the binary directly, outside an APK, so it cannot resolve
  // libc++_shared.so from an app's library path - it is pushed alongside.
  '-DAIDJ_TESTS_ON_DEVICE=ON',
  // No Gradle here, so no Oboe prefab and no need for it - the tests bring
  // their own output and decoder implementations.
  '-DAIDJ_ANDROID_PLATFORM_SOURCES=OFF',
]);

run(cmake, ['--build', buildDir, '--parallel']);

const binary = join(buildDir, 'tests', 'aidj_engine_tests');
const stl = join(
  ndk,
  'toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android',
  'libc++_shared.so',
);

run(adb, ['shell', `mkdir -p ${deviceDir}`]);
run(adb, ['push', binary, `${deviceDir}/aidj_engine_tests`]);
run(adb, ['push', stl, `${deviceDir}/libc++_shared.so`]);
run(adb, ['shell', `chmod 755 ${deviceDir}/aidj_engine_tests`]);

const passthrough = process.argv.slice(2).join(' ');
run(adb, [
  'shell',
  `cd ${deviceDir} && LD_LIBRARY_PATH=${deviceDir} ./aidj_engine_tests ${passthrough}`,
]);

run(adb, ['shell', `rm -rf ${deviceDir}`]);
