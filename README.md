# AI DJ

Give the app a playlist, press start, and it plays a continuously mixed DJ set.

**Current status: Milestone 1 (foundation) only.** There is no playlist, no
audio analysis, no queue planner and no transition engine yet. What exists is
the skeleton those things will be built on, and proof that every layer is
genuinely connected. Read [docs/known-limitations.md](docs/known-limitations.md)
before assuming any capability.

## Layout

```
apps/
  mobile/                Expo app (Android). Native module in modules/aidj-audio
  api/                   Fastify service
packages/
  core/                  Pure TypeScript: shared types, planners (M4/M5)
  engine/                C++17 audio engine. Builds and tests on a desktop host
  backend/               Convex schema and functions
docs/                    Architecture decisions, known limitations
```

## Prerequisites

- Node 20+
- JDK 17
- Android SDK with NDK 27.1.12297006 and CMake 3.22.1
- An Android device with USB debugging (minSdk 26)

Set `JAVA_HOME` and `ANDROID_HOME` before building.

## Setup

```bash
npm install
```

Convex, from `packages/backend`:

```bash
npx convex dev
```

That prints a deployment URL. Create `apps/api/.env`:

```
CONVEX_URL=https://<deployment>.convex.cloud
CONVEX_SITE_URL=https://<deployment>.convex.site
PORT=3000
```

and `apps/mobile/.env`:

```
EXPO_PUBLIC_CONVEX_URL=https://<deployment>.convex.cloud
```

`EXPO_PUBLIC_API_URL` is optional in development - the app derives the dev
machine's LAN address from the Metro host, because a physical device cannot
reach `localhost`.

For sign-in, set `AUTH_RESEND_KEY` on the Convex deployment. Without an email
sender you can set `AIDJ_ALLOW_ANONYMOUS_AUTH=true` on the deployment to enable
the anonymous provider - **development only**, it lets anyone mint a session.

## Running

```bash
npm run api:dev
```

```bash
npm run mobile:android
```

## Tests

TypeScript (API contract, envelope shape, auth rejection):

```bash
npm test
```

C++ engine (ring buffer, resampler, mixer, state machine) on the desktop host:

```bash
cmake -S packages/engine -B packages/engine/build -DAIDJ_BUILD_TESTS=ON
cmake --build packages/engine/build
./packages/engine/build/tests/aidj_engine_tests
```

The engine deliberately builds without Android. If verifying a beat tracker
ever requires a phone, the architecture has gone wrong.
