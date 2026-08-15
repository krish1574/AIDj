# Known limitations

Honest running list. Anything here is real, currently true, and not disguised
as working functionality.

## Milestone 3 / 4 (current)

### Key detection is not usable on percussion-heavy material
Measured on a real 30-track library (Galaxy S24 FE, analysis version 3): key
confidence came back at 0.00-0.16 for every music track. Tempo and beat
tracking on the same files were fine (beat confidence 0.5-0.83).

Two causes, both real rather than incidental:
- chroma is computed by folding FFT bins onto pitch classes, which has poor
  resolution in the low octaves where the tonic usually sits. A constant-Q
  transform is the correct tool and is not implemented.
- the library is dominated by garba and DJ edits, which genuinely have weak
  and shifting tonality.

The consequence is *correct* behaviour rather than a silent failure: the queue
planner multiplies its key term by confidence, so key contributes nothing on
this material and the planner falls back to tempo, energy and loudness. But it
does mean harmonic mixing is effectively unavailable today, and the number
should not be presented to users as if it were meaningful.

### Analysis runs at roughly 8x real time
Measured, not estimated: 130 s of audio takes ~17 s on a Galaxy S24 FE. A full
pass over 7 hours of long mixes is therefore around an hour of sustained CPU.

Removing the per-frame logarithms from the chroma loop made no measurable
difference, so the cost is in decoding and the FFT. Anyone optimising should
profile those and ignore the feature extractors.

### Beat confidence took four attempts to get right
Recorded because the failures are instructive and the metric is load-bearing -
the transition planner is required to degrade when it is low. See the comment
block in `BeatTracker.cpp`. Versions that compared beats against neighbouring
frames all scored noise highly, because the dynamic-programming tracker places
beats on local maxima by construction. The working version measures normalised
autocorrelation of the onset envelope at the beat period.

### Tempo octave errors still occur
`Over the Horizon` (a ringtone) reports 194 BPM where the true value is
probably ~97. Confidence was 0.10, so the system flags it as unreliable, and
`alternateBpm` carries the other reading - but nothing yet acts on that.

## Milestone 2

### Playlists are local only
The library and playlists live in SQLite on the device. Nothing syncs to Convex
yet - that is Milestone 7. A factory reset or an app uninstall loses them.

### Track identity is lazy, so cross-device matching is incomplete
`content_hash` is computed only when a track first enters a playlist, because
hashing reads 2 MiB per file and doing that for a whole library would stall the
scan. Tracks that are merely *listed* have `content_hash = NULL` and cannot yet
be matched against the same file on another device.

### A rescan can empty a playlist
Tracks absent from a scan are deleted, and that cascades to playlist entries.
There is a guard against the common disaster - an empty scan on a device that
previously had music is ignored rather than treated as a mass deletion - but a
genuinely removed file still disappears from every playlist containing it. This
is correct behaviour (an unplayable entry is worse) yet it is destructive and
currently silent, with no "3 tracks are missing" prompt.

### MediaStore is the only source
Music that MediaStore has not indexed is invisible: files in app-private
storage, some SD card states, and anything the media scanner has not yet picked
up. There is no "add file manually" path in the library - the engine debug
screen's picker is the only way to reach an arbitrary file, and it does not add
to the library.

### Reordering is buttons, not drag-and-drop
Playlist reordering uses up/down controls. The ordering model underneath
(sparse integer keys with compaction) is the real one and supports arbitrary
moves; only the gesture layer is missing.

### Nothing here plans a DJ set
Ordering a playlist by hand is not queue planning. The Queue Planner needs BPM,
key and energy from analysis, and does not exist until Milestone 4.

## Milestone 1

### The product does not mix anything yet
There is no playlist, no analysis, no queue planner and no transition engine.
The only thing that moves gain between two voices is the **developer
crossfade**, reachable solely from the debug screen. It is a fixed-length
equal-power fade with no beat alignment, no tempo matching and no EQ.

It exists to prove that gain automation reaches the audio callback
sample-accurately and that the two-voice summing path is correct under real
threading. Calling it a transition would be a lie: real transitions need beat
grids (Milestone 3) and the transition planner (Milestone 5).

### ~~The resampler is not release quality~~ - FIXED
Replaced with a Kaiser-windowed sinc polyphase FIR with interpolation between
filter phases. Measured on-device at 44.1 -> 48 kHz: spurious content -96.7 dB
at 1 kHz and -91.3 dB for a 20 kHz tone, versus roughly -60 to -70 dB for the
Catmull-Rom interpolator it replaced. Downsampling 96 -> 48 kHz rejects
above-Nyquist content at -119.9 dB.

Two findings from that work are worth keeping, because both were invisible to
the obvious test:
- a 1 kHz test tone cannot detect near-Nyquist imaging at all. With the cutoff
  sitting exactly at Nyquist, 1 kHz measured fine while a 20 kHz tone produced
  spurious content at -24.8 dB.
- filter length was not the limit for that case. Doubling the taps changed
  nothing; phase quantisation was the limit, and interpolating between phases
  moved it from -59.9 dB to -91.3 dB.

### The output limiter has no lookahead
`Mixer::softLimit` is a soft-knee waveshaper. It cannot catch an inter-sample
peak that only exists after reconstruction. Acceptable while the only thing
being summed is two voices at known gains; a proper lookahead limiter belongs
with the real transition engine.

### Audio device changes are detected but not recovered from
`OboeOutput::onErrorAfterClose` logs a torn-down stream (headphones unplugged,
Bluetooth connected) and sets a flag. Nothing re-opens the stream yet, because
there is no session to restore into until Milestone 6. Today the symptom is
that audio stops until the user presses play again.

### Reloading a voice can click
`Engine::loadVoice` posts a deactivate command and then resets the voice's ring
buffer. The audio thread may not have drained that command yet, so it can read
one more buffer from a ring whose indices have just been reset. The indices are
atomics, so there is no tearing and no crash - the worst case is a single
audible click when replacing a source that is already playing.

Not fixed in Milestone 1 because the correct fix is a proper voice-handoff
protocol (the audio thread acknowledges the release before the producer touches
the buffer), and that belongs with the continuous player in Milestone 6, which
is the first thing that swaps voices while audio is running.

### No background playback
There is no foreground service, so playback does not survive the app going to
the background. The permissions are declared in `app.json` in anticipation, but
the service itself is Milestone 6 work.

### Voice position is decode-based, not output-based
`VoiceStatus.positionMs` counts frames the mixer pulled from the ring buffer.
It does not subtract the output latency still sitting in the device buffer, so
it runs a few tens of milliseconds ahead of what the listener actually hears.
Fine for a progress bar; **not** good enough to schedule a beat-aligned
transition against, which will need the timestamp Oboe can provide.

### iOS is architected for, not built
`IAudioOutput` and `IDecoder` are the only Android-specific seams, and the
engine core builds and is tested on a desktop host. But no iOS implementation
exists and none has been compiled.

### Auth cannot be verified without an email sender
Convex Auth is configured with the Resend provider, which needs
`AUTH_RESEND_KEY` on the deployment. Until that is set, sign-in cannot be
exercised. An `Anonymous` provider is available behind the
`AIDJ_ALLOW_ANONYMOUS_AUTH` deployment variable for development only - it lets
anyone mint a session and must never be set on production.

## Accuracy expectations for analysis (Milestone 3, not yet built)

Recorded now so the numbers are not quietly revised later to match whatever we
get:

- **BPM / beat grid** - reliable on 4-on-the-floor electronic material,
  degrades on rubato, live drums and swing. Half/double-time ambiguity is
  expected. Confidence is stored and the planner must act on it.
- **Downbeat** - harder than beat tracking. Expect 75-85% on dance music. A
  downbeat wrong by one beat is audible.
- **Key** - roughly 70% exact, ~85% within a Camelot-adjacent slot. Usable as
  one weighted factor, never as a gate.
- **Structure** - boundaries are reliable, *labels* are not. We will emit
  sections with energy and novelty, and deliberately not claim to identify
  choruses.
- **Vocal activity** - the weakest signal by far. No stem separation on-device
  in V1. It is a heuristic producing a probability curve, weighted low, and
  used only to prefer an instrumental overlap when one is clearly available.
