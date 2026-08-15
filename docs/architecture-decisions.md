# Architecture decisions

Short records of the choices that would be expensive to reverse, and why they
were made. Decisions that changed during Milestone 1 are marked.

## ADR-1: Expo with prebuild, not bare React Native CLI

We need a custom C++/JNI audio module, so Expo Go is out from the start. Expo's
prebuild plus the Expo Modules API gives us full native projects *and* keeps
EAS builds, config plugins, and the `expo-*` module ecosystem. `android/` is
generated and gitignored; the native module lives in `apps/mobile/modules/`,
which survives `prebuild --clean`.

## ADR-2: Oboe for output

Two decoded streams have to be mixed with independent gain, and later with EQ
and time-stretch. No React Native audio library can do that. Oboe gives the
low-latency AAudio path with an OpenSL ES fallback and absorbs the Android
device-quirk matrix.

## ADR-3: NDK MediaCodec for decoding, not FFmpeg

FFmpeg would mean an LGPL/GPL relinking obligation, roughly 10 MB per ABI, and
software decoding that costs battery. `AMediaCodec`/`AMediaExtractor` are
hardware-accelerated and cover mp3, aac/m4a, flac, wav, ogg and opus - the
realistic local-library set.

Cost accepted: the PCM output encoding varies by device (16-bit integer on
most, float on some) and is only knowable from the *output* format after the
first format-changed event. Handled in `MediaCodecDecoder::pumpCodec`.

## ADR-4: DSP written in-house, not Essentia or aubio

Essentia is AGPL-or-commercial and cross-compile-hostile; aubio is GPL. Every
algorithm we need is published and individually small. This is the single
biggest schedule risk in the project and is named as such - it is accepted for
licensing and binary-size reasons, and mitigated by the engine building and
being tested on a desktop host.

## ADR-5: Queue and transition planning in TypeScript, DSP in C++

The split is by *data*, not by language preference: planners operate on track
metadata, not on samples. Keeping them in `packages/core` as pure TypeScript
means ordering logic is unit-tested in Node against fixture profiles, with no
device and no audio. Only sample-rate work lives in C++.

## ADR-6: The playback state machine lives in C++

The authoritative state sits next to the sample clock. JavaScript holds a
read-only mirror fed by polling. A mutable JS-side copy of "are we playing"
would race the audio thread, and that race is what produces gaps. There are no
`isPlaying`/`isTransitioning` booleans in the codebase; the UI branches on a
single state value.

## ADR-7: npm workspaces, not pnpm (changed during Milestone 1)

The proposal said pnpm. `corepack enable` fails on this machine without
administrator rights (`EPERM` writing to `C:\Program Files\nodejs`). npm
workspaces cover everything we need from a workspace tool, and avoid making an
admin install a prerequisite for cloning the repo. Metro is configured for the
hoisting layout in `apps/mobile/metro.config.js`.

## ADR-8: Convex Auth as the only identity issuer

Convex Auth mints the JWT; the Fastify service verifies it against the
deployment's JWKS and reads the subject. Node keeps no user table. Duplicating
an identity model across two services is the classic two-sources-of-truth
mistake.

## ADR-9: The Node tier is deliberately thin, and that is a fair criticism

In V1 Fastify only does health and identity echo. It exists because outbound
third-party metadata lookups need connection pooling, caching and a secret that
must not reach a client, and because licensed-catalog integrations will land
there later. The rule is: **never proxy Convex CRUD through Fastify.** If a
route only forwards to Convex, delete it.

## ADR-10: Fixed 48 kHz stereo engine format

Everything is resampled on decode. The mixer must never reason about two voices
at different rates mid-transition. See `known-limitations.md` for the current
resampler's quality ceiling.
