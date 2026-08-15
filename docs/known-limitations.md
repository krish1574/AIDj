# Known limitations

Honest running list. Anything here is real, currently true, and not disguised
as working functionality.

## Milestone 1 (current)

### The product does not mix anything yet
There is no playlist, no analysis, no queue planner and no transition engine.
The only thing that moves gain between two voices is the **developer
crossfade**, reachable solely from the debug screen. It is a fixed-length
equal-power fade with no beat alignment, no tempo matching and no EQ.

It exists to prove that gain automation reaches the audio callback
sample-accurately and that the two-voice summing path is correct under real
threading. Calling it a transition would be a lie: real transitions need beat
grids (Milestone 3) and the transition planner (Milestone 5).

### The resampler is not release quality
`packages/engine/src/dsp/Resampler.cpp` uses Catmull-Rom cubic interpolation.
At the common 44.1 kHz to 48 kHz ratio its imaging artefacts sit roughly
60-70 dB down. That is fine for bring-up and for verifying the playback path.

**It must be replaced with a windowed-sinc polyphase FIR before any human
listening evaluation of transition quality**, because resampling artefacts
would contaminate exactly the judgement that evaluation is meant to make.

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
