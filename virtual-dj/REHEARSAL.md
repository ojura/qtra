# Silent karaoke and speech rehearsal

Run the repeatable logic checks from the repository root:

```sh
python3 -B virtual-dj/rehearsal.py
python3 -B virtual-dj/rehearsal.py --suite karaoke --json
python3 -B virtual-dj/rehearsal.py --suite speech --verbose
python3 -B virtual-dj/rehearsal.py --suite voice-render --json
```

The default runs the karaoke, speech, voice-render and render-cli suites. `--json` emits one structured result; `--verbose` includes individual test output. `--timeout SECONDS` bounds the selected suites, defaults to 60 seconds, and accepts values greater than zero up to 600. Exit 0 means every selected suite ran and passed, every guard check passed, and cleanup completed. Skipped or empty suites, unexpected side-effect attempts and leaked workers fail the run. Exit 1 reports a rehearsal failure; invalid CLI arguments use argparse's exit 2.

Use this as a standalone Python process. Its audit hook stays installed through cleanup and process exit. A caller should launch the script as a child rather than import and run it inside a production server.

## What the command exercises

The runner imports its four named suites after installing the guard. Karaoke cues and dedication phrases are original fixtures. Speech uses fake prepared clips, fake playback processes and fake Pulse streams. Renderer tests use fake executors and private PCM fixtures; CLI tests cover cleanup, explicit export, failure reporting and the narrow process permission. Temporary history, database and WAV files live below one private temporary directory and are removed after the run. These tests run independently of the party server, Spotify, provider logins, microphones and audio devices.

| Area | Cases and required result |
| --- | --- |
| Recording and occurrence | A cue sheet follows its first accepted matching UID, survives catalogue relinking and persistence, and waits when the same URI appears with a different UID. Snapshot reads do not change that binding. |
| Clock and zero position | An explicit numeric zero is valid. Missing, boolean, nonfinite, negative or future clock values hold the lyrics and cannot admit speech. Explicit invalid `clockValid` metadata also holds, even when a compatibility field contains numeric zero. Backward and forward seeks use the new measured anchor. |
| Pause, buffering and expiry | Explicit pause or buffering overrides a legacy playing flag. Spotify interpolation ends at the source observation's fixed ten-second deadline. Repeated snapshots cannot extend it. |
| Preparation before the turn | An automatic dedication uses a clip ready before its exact occurrence started. Missing or late preparation becomes `missed`; late synthesis cannot revive that occurrence. |
| Cancellation | Pause, interruption, occurrence change, output-device change and disable invalidate the queued or active job. Returning to the former track/device or resuming does not revive it. Removed or disabled preparation is closed when it finishes. |
| Speech lifecycle | Queueing, player startup, completion, failure, cancellation and a missed opening remain distinguishable. Missing completion evidence or a nonzero process result fails. |
| Mixer ownership | Fake fades affect the selected Spotify stream, restore owned channel levels, stop on a changed output, and preserve a newer host volume change. |
| Recovery and bounds | Repeated observations and restart do not repeat a dedication. Stable manual request IDs are idempotent. Preparation state is bounded to the next three requests plus the current one. |
| Exceptional attention | Missing local capability and failed upcoming preparation expose explicit attention. A manual preparation failure is recorded in lifecycle history. Ordinary requests and those attention entries do not launch another provider. |

The runner also checks its own guard before the suites. Process and HTTP probes enter the real Python dispatch paths, with inert sentinels behind the audit hook so a broken hook cannot execute the probe. Other entry paths are checked with synthetic audit events. These checks cover process execution, HTTP, sockets/DNS, working-directory changes, writes outside the private directory, relative writable opens and mutation through stdout's descriptor. A private file/rename/delete and SQLite operation must still work.

An unexpected denied operation makes the whole run fail even when application code catches the exception. Subprocesses, network and local-service sockets, media-device opens, filesystem links and nonprivate writes are disabled for test worker threads as well as the main thread. Relative writable opens are rejected because Python's `open` audit event omits `os.open`'s directory descriptor.

## Reading the result

The JSON report includes `status`, per-suite test counts/failures/skips, `guardChecks`, `sideEffectAttempts`, `threadsRemaining`, `errors`, `privateOutputs` and elapsed time. `audibleVerification` is always `unperformed` for this silent command. A failed report retains the captured test log; successful JSON includes it only with `--verbose`.

A timeout prevents another selected suite from starting. The runner prints through the original output streams, keeps the guard active, and exits nonzero if a worker remains. When cleanup runs while a worker is still alive, `privateOutputs` is `cleanup_unresolved` and `privateDirectory` identifies the path that needs inspection after the process has exited. A normal successful run reports `cleaned`.

The scope is scheduling, identity, clock behavior, mixer commands, file-render command validation and failure handling. Native stage rendering has its own [renderer checks](../docs/party-stage.md). The file command below measures actual synthesized waveforms. Pronunciation and intelligibility on the intended speaker remain separate observations.

## File-only speech measurements

Run actual Croatian synthesis and an offline synthetic-bed mix:

```sh
python3 -B virtual-dj/render_voice.py --json
```

The original default phrase is “Antonio, ovaj refren je za tebe. Pozdrav cijeloj ekipi.” To supply different original text, pass it through stdin:

```sh
printf '%s\n' 'Antonio, pozdrav cijeloj ekipi.' | python3 -B virtual-dj/render_voice.py --text-stdin --json
```

The command uses the existing `CroatianVoice` preparation algorithm. It normalizes the phrase, shortens whole words when required by the duration limit, synthesizes with Croatian espeak-ng, and converts the result with the existing ffmpeg filters. The report retains the supplied text and the actual last synthesized text.

The parent audit hook permits only the resolved espeak-ng and ffmpeg executables with the reviewed argument lists, private WAV paths, fixed working directory and minimal environment. Each permission belongs to one launching thread and one process. espeak-ng receives text on stdin and writes a WAV with `-w`; ffmpeg receives closed stdin, uses the file protocol and emits mono 48 kHz PCM16 WAV. Every other process, network connection, media-device open and nonprivate write is rejected. The logic command above keeps its blanket process denial.

Waveform measurements include duration, sample format, RMS and peak dBFS, nonzero samples, PCM endpoint counts and mixed-sample limiting. The original synthetic bed contains 220, 330 and 440 Hz tones. Raised-cosine gain changes duck it by 10 dB over 0.48 seconds and restore it over 0.88 seconds. The report measures gain over matching source-frame windows, the final restored gain, the largest envelope step and the voice-to-ducked-bed RMS ratio. It does not open Spotify or an audio player.

Both WAVs are removed after measurement. An explicit export writes `voice.wav` and `mix.wav` into a new directory:

```sh
python3 -B virtual-dj/render_voice.py --export /tmp/virtual-dj-voice-preview --json
```

The export parent must exist, and the requested directory must not exist. An export failure removes the owned partial files. The JSON result contains the exported paths only when export succeeds; ordinary runs return measurements and `privateOutputs: cleaned`. Errors include the failing stage and process status when available. Unexpected denied operations fail the result even when lower-level code catches their exception. Run this command as its own Python process because its audit hook remains installed through exit.

File measurement leaves `pronunciationVerification` and `audibleVerification` as `unperformed`. Listening observations can be added only after an explicitly authorized playback check.

### Measured default phrase

The default phrase rendered without shortening on 2026-11-17, using the local espeak-ng binary and `/usr/bin/ffmpeg`. Both commands exited successfully. The file command passed all 18 guard checks, recorded zero unexpected side-effect attempts, and removed its private WAVs. The combined logic command passed 80 tests: 19 karaoke, 31 speech, 16 renderer and 14 CLI tests.

| Measurement | Prepared voice | Voice with synthetic bed |
| --- | ---: | ---: |
| Duration | 3.752333 s | 6.612333 s |
| Samples | 180112 | 317392 |
| Sample format | 48 kHz, mono, PCM16 | 48 kHz, mono, PCM16 |
| RMS | -20.3672 dBFS | -21.4239 dBFS |
| Peak | -3.0000 dBFS | -2.4649 dBFS |
| PCM endpoint samples | 0 | 0 |
| Nonzero samples | 138507 | 317282 |

The measured bed gain was -9.99984 dB during speech and 0 dB before ducking and after restoration. The 0.48-second duck and 0.88-second restore ended at gain 1.0, with a maximum per-sample envelope step of 0.00004662. The voice-to-ducked-bed RMS ratio was 12.3782 dB. The mix required zero sample limiting. These measurements establish the file levels and timing; pronunciation and speaker intelligibility remain unperformed listening observations.

## Observed morning dedication: request 302

On 2026-11-17, the audience renewed the missed Zdenko dedication. The exact line
“Zdenko, ova je za tebe!” was rendered with `hr-HR-GabrijelaNeural` and played once,
for **2.904 seconds**, over **q162, The Selecter, On My Radio**. q161 was initially
considered, but its natural transition happened before any mixer invocation.
Fresh preflight bound the only playback to q162 and the actual `juraj-legion`
device. The speech process exited with code 0. Spotify stream 2279 stayed on the
same Bluetooth sink 2329; both channel levels followed **65536 → 44649 → 65536**
through the existing 10 dB duck and restoration. Global speech remained disabled.

The completed MP3/WAV were removed after the outcome was recorded; the natural-voice environment remains
available for the current live session. The earlier silent espeak-ng measurements
retain their separate results, and the overnight cancelled performances remain
cancelled.

### Reuse the natural Croatian preparation path

For a newly requested line, set `dedication_text` to its exact wording and render
quietly into a new temporary directory:

```sh
dedication_dir=$(mktemp -d /tmp/larics-dedication-XXXXXX)
/tmp/larics-zdenko-dedication/venv/bin/python -m edge_tts \
  --voice hr-HR-GabrijelaNeural --text "$dedication_text" \
  --write-media "$dedication_dir/voice.mp3"
ffmpeg -nostdin -loglevel error -i "$dedication_dir/voice.mp3" \
  -af 'highpass=f=80,lowpass=f=8500,loudnorm=I=-19:TP=-3:LRA=6,afade=t=in:st=0:d=0.03' \
  -ar 48000 -ac 1 -c:a pcm_s16le "$dedication_dir/voice.wav"
```

The retained environment supplies the Edge neural renderer; it has no automatic
connection to the service's speech scheduler. Measure the WAV before admission
and retain the existing maximum of 12 seconds. From the repository root with
`PYTHONPATH=tools/party_dj`, use `speech.PreparedSpeech` for the prepared file and
the existing runtime call
`SpotifyMixer().play(prepared, valid, stop, on_started=...)`.
The `valid` callback must keep checking the exact current UID and device, a fresh
actual source observation within ten seconds, valid clocks, playing status, and
explicit false pause/buffering. Keep global speech disabled for a one-off. The
mixer selects the existing Spotify stream/output, scales speech to its original
level, ducks only that stream, and preserves a newer host volume change. Read its
process result before reporting completion, record the request's outcome in the
maintained session notes, and remove its completed audio and temporary result files.

## Prepare a dedication

1. Keep the host's current playback decision. An explicit pause remains in force throughout silent rehearsal.
2. Choose the requested occurrence and exact wording, including the name. Keep ordinary tracks' dedication text empty.
3. Prepare while that request is still upcoming. Inspect its preparation entry: `requestId`, `occurrenceUid`, `text`, `status`, `ready`, and, when available, `seconds` or `error`. A failed preparation needs a concrete correction before the occurrence begins.
4. At admission, require a fresh playing observation and a nonempty string occurrence UID. Use the actual source `observedAt`, progress, pause/buffering flags and output-device identity. Derive the occurrence start from the observed progress; receiving the first observation late must not make a late clip appear early.
5. An automatic clip that misses that opening stays `missed`. A separate manual dedication needs a new, deliberate request and the current verified occurrence. It may prepare on demand, then must revalidate the occurrence, device and cancellation generation before playback.
6. Read the lifecycle result. `queued` means accepted work, `starting` means a prepared job reached the mixer, and `speaking` means the mixer reached its startup callback. `completed` carries successful process-exit evidence with `audibleVerification: unperformed`. `failed`, `cancelled` and `missed` retain their actual outcomes.
7. Preserve cancellation and idempotency across retries. Reuse the request ID when recovering the same request; use a new ID only for a newly intended announcement. A pause, skip or device change must not be undone to salvage old speech.

The new-install server policy keeps speech disabled until configured for an intended session. The controller can inspect preparation and exceptional attention without treating availability as permission to play.

## Prepare karaoke

1. Select the exact recording and usable timed lyric material before inviting a performer. Check the participant order, duration, cue bounds and pronunciation or syllable splits where relevant.
2. Preload against the recording URI, then bind to the first accepted matching occurrence UID. Persist that binding. Catalogue relinking may preserve the UID; a repeat of the same URI has a different occurrence and needs a deliberate new turn.
3. Use one source clock. For Spotify, `clockValidUntil` is the actual `observedAt + 10000` in epoch milliseconds. A rendering timestamp must never renew that deadline. Explicit manual-clock sheets retain their separate start/pause/seek semantics.
4. Check the start, exact zero, pause, buffering, backward seek, forward seek, expiry, reconnect and repeated-recording cases. A held or stale clock keeps a stable position. The native renderer interpolates only a playing snapshot and caps elapsed time at the supplied deadline.
5. Inspect the privately rendered stage at the intended viewing size before the performance is ready. Confirm the words, active cue, title and performer order. Keep this visual observation separate from the Python suite result.

The maintained [karaoke format](../tools/party_dj/README.md#karaoke) defines cue input, and the [playbook](PLAYBOOK.md#karaoke) covers the performance procedure.

## Server and renderer integration

The accepted-observation path calls `Karaoke.observe(current, connected, stamp)` before rendering snapshots or notifying audio. Save `Karaoke.saved()` when `observe` returns true. Snapshot generation is a pure read and deep-copies the public cue data.

Spotify snapshots expose `clockState` as `running`, `paused`, `buffering`, `invalid`, `stale`, `unmatched` or `disconnected`. Invalid clocks have `clockValidUntil: 0`; stale clocks retain their original deadline and report `status: held`. An unmatched or disconnected sheet waits. For a playing snapshot, native interpolation adds `max(0, min(now, clockValidUntil) - sampledAt)` before applying its usual cue position handling.

The bridge exposes boolean `nowPlaying.clockValid` and `playbackState.clockValid`. A present value other than boolean true holds the consumer, regardless of numeric compatibility fields. Karaoke keeps its last accepted measured position and does not replace that cache with an invalid fallback. Speech also respects invalid metadata from the raw playback state. An absent flag preserves older-client compatibility while the consumers still validate position and observation time. The bridge flag does not renew the karaoke deadline.

Pass accepted source observations and the playback state to `DedicationAudio.observe(current, requests, playback_state=...)`. The playback state supplies the output-device identity and explicit pause/buffering flags. Do not replace the source timestamp with the time a controller happened to render or poll it.

`SpotifyMixer.play(prepared, valid, stop, on_started=...)` calls the startup callback only after player creation succeeds and the job is revalidated. Its successful return is process evidence. Keep that evidence when publishing completion; leave audience verification to an actual observed performance.

## Exceptional provider handoff

Spotify is the normal music source. A preparation failure is a reason to inspect the failure; it does not automatically open YouTube or another player. Use an external source only for an explicitly coordinated need with prepared material, participant order, visibility, timing and a return plan.

The [playbook's source-handoff procedure](PLAYBOOK.md#a-rare-external-source) assigns one owner and requires observed player state at each transition. Rehearse those decisions with fixtures. This command never opens a provider, contacts the production controller, pauses or resumes Spotify, or changes audio routing. Preserve the host's current playback decision while rehearsing.
