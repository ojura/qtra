# LARICS / LYRICS

A local party desk for Spotify, with a native qtra show and a phone-friendly song
request page. Guests can search the actual Spotify catalogue, request a track,
add a dedication, vote on the mood, and talk to the live DJ. The native display
reacts to system audio, changes its geometry and introduces robot deliveries.
Photographs and stories from the LARICS Spomenar are an optional visual source.
The reusable operator entry is [Virtual DJ](../../virtual-dj/README.md), with
queue workflows, audience guidance and silent performance rehearsals.

## Start the party

From the qtra repository:

```sh
python3 tools/party_dj/launch.py start --fullscreen
```

The command reuses the running service and qtra host. Its JSON result gives the
guest URL, service status and native module IDs. Phones must be on a network that
can reach the laptop. Scan the QR on the display, or open the printed URL.

The laptop keeps its 600-pixel search and chat sidebar open, with 29-pixel text.
Press **R** to focus the bottom Magic box, which accepts song searches or messages.
Suggestions float over the conversation without moving it or the composer.
Selecting a song turns that same box into an optional dedication; Cancel restores
the search. Enter sends, and Shift+Enter adds a line. The labelled button also
sends. **Escape** or the small × recalls the latest own message before acceptance;
another draft is preserved. **F11** toggles fullscreen. A name and song dedication
are optional. Read receipts and work outcomes are separate: active, waiting,
blocked, completed and cancelled work have their own indicators.

Speech is disabled on new installs until deliberately configured for a session.
When enabled, a nonempty dedication must be prepared before its exact requested
occurrence starts. Its lifecycle distinguishes prepared, queued, starting,
speaking, completed, failed, cancelled and missed work. A missed opening remains
missed; late synthesis does not revive the old dedication.

The mixer follows the actual Spotify stream, ducks it by 10 dB, and restores its
owned level after the phrase. Speech volume follows the original stream level
and is capped below full volume. Speech lasts at most 12 seconds; long messages
remain complete on screen and are shortened at word boundaries for speech.
Empty dedications stay silent.

Speech uses `espeak-ng`, `ffmpeg`, `pactl` and `paplay`. On this laptop, the
matching eSpeak NG command is unpacked under `build/party-audio-deps/` and uses
the installed engine and Croatian voice data. On another desktop, install the
normal eSpeak NG and PulseAudio command packages. Public state reports
`speech.available`, `speech.enabled`, and the last result.
The DJ can turn spoken dedications off or on with
`djctl.py configure --json '{"speechEnabled":false}'` (or `true`).

The mixer follows the actual native Spotify stream and uses the same audio
output. A song skip, pause or output change cancels speech and returns the owned
stream level. A newer host volume change takes precedence. Persistent occurrence
history prevents duplicate speech across restarts; the song already playing at
startup stays silent. See [REHEARSAL.md](../../virtual-dj/REHEARSAL.md) for guarded
logic tests and actual file-only synthesis/mix measurements. Successful process
completion, waveform quality and an observed audible performance are separate
results.

```sh
python3 tools/party_dj/launch.py status
python3 tools/party_dj/launch.py start --restart --fullscreen
python3 tools/party_dj/launch.py start --request-url http://192.168.1.115:8787
python3 tools/party_dj/launch.py stop
```

`stop` stops the request service and releases its Spotify control connection.
The qtra host keeps rendering; it may also contain other loaded modules. A
restart preserves requests, chat and settings in SQLite. `--service-only`
starts the request page without opening qtra. `--data` and `--port` select an
alternative state directory and HTTP port.

### Prerequisites

Spotify should already be open on the intended desktop device; preserve its
current play/pause state. The service
prefers Spotify's own loopback debugging endpoint at port 9224. To enable it,
quit Spotify normally and reopen it with:

```sh
spotify --remote-debugging-port=9224 --remote-debugging-address=127.0.0.1
```

When that endpoint is unavailable, the service uses a signed-in Spotify browser
tab through the existing CDP daemon at `http://127.0.0.1:7799`. Spotify Connect
continues controlling the currently active playback device. Search navigates
this controller tab to Spotify's normal search results. Keep the tab signed in
and keep the CDP daemon available while this fallback is in use. The state
reports `spotify.mode` as `native` or `native-via-web-controller`.

The controller discovers the running Spotify application's React service
registry and uses its PlayerAPI. These application internals can change with a
Spotify update. A failed connection is shown in the guest interface; queued
requests are retained while the service reconnects. An ordinary observation
does not create a controller tab. Use the explicit setup procedure in
[Spotify transport](../../docs/spotify-transport.md) when recovery needs one.

Install Python dependencies into the repository's ignored build directory:

```sh
python3 -m pip install --target build/party-deps -r tools/party_dj/requirements.txt
```

`launch.py` adds that directory to the service's Python import path. To build
the native modules, use the instructions in
[docs/party-stage.md](../../docs/party-stage.md). The show uses the existing
system audio monitor and QtNetwork support.

## Working the desk

`virtual-dj/dj.py` provides fresh status, full queue, guarded ordering, request
and event-watching workflows. `djctl.py` remains the low-level API entry. Both
read the local admin token and call the running service. The guest
page receives the public state and chat only. Reading the event stream or
sending an admin action records the DJ as present for 90 seconds; a live operator
should poll regularly. If the operator stops, the interface invites guests to
leave a message, and the automatic visual show continues.

```sh
python3 tools/party_dj/djctl.py events --after 0 --wait 25
python3 tools/party_dj/djctl.py state
python3 tools/party_dj/djctl.py search 'Dino Dvornik'
python3 tools/party_dj/djctl.py reply --json '{"requestId":"dj-welcome-001","text":"Tu sam. Što se sluša?","caption":true}'
python3 tools/party_dj/djctl.py cue 'Robot je upravo zatražio još jedan refren.'
python3 tools/party_dj/djctl.py speak --json '{"requestId":"spoken-dedication-001","text":"Ova je za cijelu ekipu."}'
python3 tools/party_dj/djctl.py scene --json '{"mode":"auto","effect":3,"energy":0.75,"holdSeconds":180}'
python3 tools/party_dj/djctl.py book --json '{"filename":"p0319-robot-drummer-photo.jpg","seconds":35}'
```

After each event read, carry `lastId` into the next `--after`. Events include
guest messages, requests, successful queue additions, mood votes and track
changes. A reply can include `readThrough` with the guest chat message ID. Its
`requestId` makes a repeated submission return the same message. `caption:true`
also schedules the reply on the native screen.

Record what is actually happening to a guest request:

```sh
python3 virtual-dj/dj.py work active 93 --summary 'Checking the requested transition'
python3 virtual-dj/dj.py work waiting 93 --summary 'Waiting for native review'
python3 virtual-dj/dj.py work completed 93 --outcome 'The change is deployed' --evidence 'Actual commit and deployment observation'
```

The authenticated `POST /api/admin/work` accepts `messageIds`, `state`, `summary`,
`outcome`, `evidence` and optional `leaseSeconds`. The batch is validated before
any change. `completed` requires an outcome and evidence; `cancelled` requires a
reason. Durable records survive restart and appear in each chat row's `work`
and the `dj.messageWork` map, so old displayed messages can update without new
chat text. Active lease expiry yields `effectiveState:unconfirmed`, retaining
the stored work. Read receipts, acceptance and completion remain distinct.

The legacy `processing` endpoint remains compatible for explicitly active IDs.
Clearing its active marker means waiting for a recorded outcome, not completed.
Legacy markers migrate to waiting; settled terminal work cannot be reactivated
by a blanket lease renewal. Use the work API for an intentional reopen.

New submissions retain caller-declared `source` (`native`, `web`, `api`, `test`),
guest ID and actual HTTP peer privately. Public chat exposes safe source labels
and separate `acceptedAt`/`readAt` timestamps. `virtual-dj/dj.py message-info ID`
reads the authenticated detail. Explicit test submissions use `role:test` and
`test_message` events. Legacy origin is unknown. Control-command events record
dispatch and returned/error outcomes, including the operator label and observed
occurrence/device; set `--operator` for the current operator.

To request a track as DJ, first search for it, then use its exact returned URI:

```sh
python3 tools/party_dj/djctl.py request --json '{"requestId":"dj-dino-001","uri":"spotify:track:704GJCYwhZ8w3xsbXXqrMC","requestedBy":"DJ","dedication":"Za ekipu koja je došla samo na jednu."}'
```

`next`, `pause` and `resume` remain low-level controls. The native play/pause
button uses authenticated `playback` with an explicit desired `paused` boolean,
`expectedCurrentUid` and `expectedDeviceId`; the bridge rechecks and verifies the
actual state around one command. The UI never retries an uncertain control.
Request additions
append to its explicit queue. Existing Spotify queue entries appear alongside
guest requests. A guest's repeated click retains the same request ID; active
requests for the same URI share a queue entry.

The native scene modes are `auto`, `orbit`, `laser`, `robots` and `quiet`.
`energy` ranges from 0 to 1. `effect` selects the existing qtra geometry:

| Effect | Geometry |
| ---: | --- |
| 0 | Audio-driven cube |
| 1 | Orbiting sphere |
| 2 | Möbius ribbon |
| 3 | Sphere and ribbon |
| 4 | Wireframe cube |
| 5 | Sphere and wireframe |
| 6 | Ribbon and wireframe |
| 7 | Sphere, ribbon and wireframe |

The audio composition is selected independently with `scene.audioStyle`:
`city` draws a spectrum skyline, `waterfall` draws flowing spectral history,
and `ring` retains the original circular show. The audio module follows this
field from the same state file without restarting capture. Start a new act with:

```sh
python3 tools/party_dj/djctl.py scene --json '{"audioStyle":"city","mode":"laser","energy":0.8}'
```

City and waterfall alternate every 10 minutes. Their start time persists across
service restarts; palette and energy changes keep the existing schedule. An
overdue act changes once on recovery. Selecting a different style starts a fresh
period. Linear acts keep legacy sphere/ribbon geometry off (`effect:0`).
See [docs/audio-acts.md](../../docs/audio-acts.md) for native controls and rendering.

In ring mode, automatic geometry changes run every 75 seconds outside a manual hold. Book
rotation runs every 105 seconds, with three original photographs for each page
preview. The service rereads the catalog on every rotation. Captions have IDs
and reading time proportional to their length, and incoming lines wait for the
current line to finish.

## Karaoke

The native karaoke layer consumes a timed syllable sheet through `state.karaoke`.
Load a UTF-8 JSON sheet with the DJ token; a sheet can be up to 256 KiB. Text
fragments include their own spaces, so highlighting never changes the text layout.
All cue intervals are absolute milliseconds from the beginning of the recording.
Use the lyrics and timing file supplied for the selected performance.

```sh
python3 tools/party_dj/djctl.py karaoke --file /path/to/timed-song.json
python3 tools/party_dj/djctl.py karaoke --json '{"command":"offset","offsetMs":250}'
python3 tools/party_dj/djctl.py karaoke --json '{"command":"stop"}'
```

A short original fixture illustrates the format:

```json
{
  "command": "load",
  "title": "Roboti pjevaju",
  "source": "manual",
  "durationMs": 5000,
  "lines": [{"syllables": [
    {"text": "Ro", "startMs": 1000, "endMs": 1400},
    {"text": "bo", "startMs": 1400, "endMs": 1800},
    {"text": "ti ", "startMs": 1800, "endMs": 2200},
    {"text": "pje", "startMs": 2400, "endMs": 2800},
    {"text": "va", "startMs": 2800, "endMs": 3200},
    {"text": "ju.", "startMs": 3200, "endMs": 3800}
  ]}]
}
```

For Spotify, set `source:"spotify"`, `trackUri` to the exact selected recording,
and `durationMs` to its duration. The sheet waits for that recording, follows
Spotify's play/pause and seek position, and returns to waiting when another song
starts. Loading during the song binds its queue occurrence, including catalogue
relinking. A preloaded request can match the service's `requestedUri`. A positive
`offsetMs` advances the lyric cues; a negative value delays them.

For an external recording, `source:"manual"` provides `start`, `pause`, `resume`
and `seek` commands; `seek` takes `positionMs`. It starts paused unless
`autoplay:true` is supplied. Sheets and transport anchors persist in SQLite.
An invalid replacement leaves the loaded sheet intact.

The renderer receives `status`, `positionMs`, `sampledAt`, `clockState` and
`clockValidUntil`. Position already includes the offset; interpolate only while
status is `playing`, capping elapsed time at the source observation's fixed
ten-second deadline. Repeated snapshot reads never extend that deadline.
The position anchor can be negative before a delayed lyric start; clamp to the
display range only after interpolation, so the note holds until that start.
`waiting`, `paused`, `held` and `finished` hold the note. Cue text and bounds stay fixed
while the note and highlight progress through each syllable.

## State and recovery

Runtime data lives in `~/.local/state/larics-dj/`:

| File | Purpose |
| --- | --- |
| `state.json` | Atomically replaced state for the native stage |
| `party.sqlite3` | Requests, chat, events and settings |
| `speech-history.json`, `speech/` | Spoken occurrence history and prepared voice files |
| `admin-token` | Local DJ control token |
| `request-qr.png` | Generated guest URL QR |
| `server.pid`, `server.log` | Request service process and diagnostics |
| `qtra.pid`, `qtra-modules.json`, `qtra.log` | Native host and loaded modules |
| `book/cards.json` | Source-linked photographs and page previews |

Requests move from `pending` to `sending` to `queued`, then `playing` and
`played`. The stored Spotify queue UID follows that same occurrence if Spotify
substitutes another catalogue URI during playback; the guest page also receives
the original requested URI. A failure known to occur before dispatch returns the request to
`pending`. A connection loss during a write produces `uncertain`; the service
reconciles it against the actual Spotify queue instead of dispatching it twice.
An item removed in Spotify is released after repeated complete queue observations
and a ten-second grace period. It can then be requested again. If Spotify later
shows that exact stored queue UID again, the original request returns to `queued`
or `playing` and emits `queue_restored`; no new copy is dispatched. A matching song
URI with a different or missing UID does not restore a removed request.

If an uncertain item remains absent, inspect the native Spotify queue and the
service log. The interface keeps its checking status so the DJ can resolve the
actual outcome before attempting another addition. Restarting does not erase
this state.

The request server listens on the local network. The intended deployment is the
party laptop and its guest network. The browser controller and qtra socket use
loopback and the user's runtime directory respectively.

If Spotify control disconnects, the service clears the observed current track
and external queue until a fresh observation arrives. It preserves guest requests
for reconciliation. Guest chat accepts the native composer's full 1500 characters
and rejects longer messages without silently truncating them.

## Verification and source context

```sh
PYTHONPATH=build/party-deps python3 -m unittest discover -s tools/party_dj -p 'test_*.py' -v
python3 -m unittest discover -s tests -p test_party_stage.py -v
node --check tools/party_dj/web/app.js
node --check tools/party_dj/spotify_bridge.js
```

Backend tests exercise request idempotency, concurrent metadata lookup, dispatch
recovery, queue removal, and chat deduplication. Native integration tests exercise
actual Qt widgets, caption timing, scene changes and state recovery. Guest
browser checks covered real search artwork, a real Spotify queue addition,
two-way chat, stale-response handling and the phone layout.

[BOOK.md](BOOK.md) records the fully ingested Spomenar edition, source links,
page mapping, selected photographs and music cues. The procedural robot motion
comes from the Spomenar viewer in `overleaf-workshop` at 7038b0c, especially
`views/pdf-viewer/src/common/prefetch-robots.ts`, `robot-dialogue.ts`,
`robot-screen-scale.ts` and `robot-structure.ts`. The native timing and comic
direction also draw on qtra's `soliloquy_intermission.cpp` and the original
Claude session 7041d14e-f0f1-4ff8-8ea6-47f6e1c84d75, including its dramaturg review.
