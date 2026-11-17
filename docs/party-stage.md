# LARICS / LYRICS party stage

`snippets/party_stage.cpp` adds a live party display and an integrated song request
and DJ chat panel to qtra. The display follows the DJ service's state file while
the existing `audio_pulse` module keeps analysing the system audio and rendering
its spectrum ring, beat shells, shockwaves and particles.

The stage supplies track metadata, a phone request QR, a queue footer, readable
caption cues, photographs from the LARICS Spomenar, and robot record deliveries.
Its orange, cyan and indigo presentation follows the book's LARICS identity and
the printed-page-34 anecdote, “Did you mean LYRICS?”

## Build and load

Build the host-matched modules and the host's generated coverage report together:

```sh
cmake --build build/release --target runtime_agent_coverage \
  agent_snippet_audio_pulse agent_snippet_party_stage \
  agent_snippet_orbit_sphere agent_snippet_mobius_ring --parallel 3
```

The coverage target matters after the host is relinked. The audio step binding
uses the generated report for that exact executable.

Start or reuse qtra, register its scene modules, and show the stage:

```sh
python3 tools/launch_party_stage.py --fullscreen
```

The launcher reuses the exact existing socket and recorded module IDs. It checks
for a same-user process with the requested binary and socket before starting a
new host. It writes `qtra.pid`, `qtra-modules.json`, and `qtra.log` beside the state
file. The native request desk is always open (`--panel` remains compatible). `--state-file`, `--base-url`,
`--socket`, `--runtime-dir`, and `--build-dir` select an alternative local setup.

For the Tab switch to native Spotify, install the small X11 event dependency into
the existing local dependency folder:

```sh
python3 -m pip install --target build/party-deps python-xlib
```

The launcher starts `tools/party_dj/window_toggle.py` once and reports its status
in `windowToggle`. It owns `window-toggle.pid`, `window-toggle.sock`, and
`window-toggle.log` beside the state file. Its `status` and `stop` commands inspect
or release the binding; they do not change playback. The Qt action works in qtra
on Wayland or X11. The return binding requires native Spotify's X11 window.

For manual runtime control, start qtra with a dedicated socket:

```sh
build/release/qt_runtime_cube --agent-socket /run/user/1000/larics-dj-qtra.sock
```

Use the [runtime protocol](protocol.md) or `tools/agentctl.py` to load these modules:

| Module | Executor | Initial request |
|---|---|---|
| `agent_snippet_audio_pulse.so` | `render` | `{"events":true}` |
| `agent_snippet_orbit_sphere.so` | `render` | `{"restore":true}` |
| `agent_snippet_mobius_ring.so` | `render` | `{"restore":true}` |
| `agent_snippet_party_stage.so` | `gui` | `{"stateFile":"/home/juraj/.local/state/larics-dj/state.json"}` |

Calling the two scene modules with `restore` registers their named actions and
leaves them inactive. The party state selects them afterwards. Modules are loaded
once and remain resident. Releasing the party module removes its widgets and
shortcuts, restores the prior menu/status-bar visibility and title, and restores
the effect actions to the states they had when the stage took control.

Party-stage requests also accept `panel:true|false` (both keep the desk open), `fullscreen:true|false`, a
`requestBaseUrl` override for the local API, an
inline `state` object, and `capture:"/absolute/path.png"`. Captures include the
native overlay and request panel. `restore:true` releases the stage. Every run
returns its current revision, scene, effect, cue, pending cue count, loaded image
status, panel state, and state-file or missing-effect errors.

`getComposerDraft:true` also returns `composerDraft`, containing the multiline
text, cursor/anchor positions, selected track and saved query. Pass that object
as `composerDraft` to restore the active Magic box intent during a hot reload.
The name field remains `partyName`; the composer is `partyMessage` and exposes
Qt's `plainText` property.

## Guest controls

- **Tab** switches qtra to fullscreen native Spotify, and **Tab** in Spotify
  returns to qtra with the same draft and focus. The helper binds Tab only on
  Spotify's managed windows; other applications retain their own Tab behavior.
  Closing or relaunching Spotify refreshes those bindings. Switching windows
  leaves the track, queue and volume alone.
  A permanent cyan **Tab: qtra ↔ Spotify** hint appears below the Magic box in
  its fixed help row, including while a song is selected.
- The native request and chat panel stays open. Click **Zatraži pjesmu · R**, or
  press **R**, to focus its bottom **Magic box**, which accepts a song, artist, or a
  message to the DJ. Typing shows debounced results above it; **Pošalji poruku (Enter)** always
  sends chat until a song is explicitly selected.
- Selecting a result shows the chosen song in the floating suggestion panel and turns
  the box into an optional dedication. **Pošalji pjesmu** sends that song and
  dedication. **Odustani od pjesme** restores the original query and cursor.
- **Shift+Enter** adds a new line. **Enter** and the Send button submit the current
  message or selected song. The multiline composer wraps and displays about four
  lines; the panel uses 29-pixel text, larger controls and proportional spacing.
  Incoming DJ replies stay in the transcript.
- A grey check beside a guest message means the DJ has marked it read. Receipt
  refreshes update existing messages without adding duplicates or verbose send
  confirmations beneath the composer.
- An orange dot means the DJ is actively working on that message. It follows the
  explicit `dj.processingMessageIds` array from the chat API and clears when the
  work ends. The fixed presence row also says that the DJ is working.
- Incoming messages scroll smoothly while the transcript is following its
  bottom. Sending a guest message follows it to the bottom as well. Typing,
  scrolling or clicking interrupts that motion. After about
  20 seconds without interaction, the conversation eases back to the latest
  message; ongoing typing or dragging keeps the current position.
- The sidebar is 600 pixels wide. Suggestions float over the bottom of the
  conversation, directly above the composer, and disappear when there are no
  results. The transcript and composer keep the same bounds throughout loading,
  selection, empty results and errors. While typing, existing results stay in
  place with a quiet activity bar;
  stale choices are disabled until the new results arrive. Hover highlights and
  pixel scrolling make the list easier to scan. Long titles are elided with full
  text available in tooltips.
- **Escape** does nothing. The panel has no close button and remains visible.
- **F11** toggles fullscreen.
- The QR opens the separate guest page on phones. The native panel uses the
  loopback service URL, independently of the phone-facing address.

The panel uses QtNetwork, which qtra already links. Search is debounced, stale
search replies are discarded, and retrying an interrupted submission retains its
request UUID. Incoming and outgoing chat messages are deduplicated by ID and
rendered as escaped text. Only fetched history advances the chat read cursor.

## State file

The default file is `~/.local/state/larics-dj/state.json`. The service should replace
it atomically with a complete JSON object. The stage polls four times per second,
retains the last valid state through partial or malformed writes, and reports the
error in its runtime status. It reads at most 1 MiB.

| Field | Meaning |
|---|---|
| `revision` | State version, echoed in stage status |
| `nowPlaying` | `title`, `artist`, `uri` or `id`, `playing`, `progressMs`, `durationMs`, `observedAt` in epoch milliseconds |
| `queue` | Tracks with `title`, `artist`, `uri` or `id`, `requestedBy` |
| `scene` | `mode`, `energy` from 0 to 1, `palette`, `effect` from 0 to 7, `graphicsScale` from 0.75 to 2 |
| `caption` | `id`, `text`, `seconds`, `createdAt` in epoch milliseconds |
| `bookMoment` | `imagePath`, `title`, printed `page`, optional `expiresAt` in epoch milliseconds |
| `requestUrl` | Guest address printed below the request affordance |
| `requestLocalUrl` | Native API base, default `http://127.0.0.1:8787` |
| `qrPath` | Local QR image path |
| `mood` | The display uses `label`; the service owns the voting logic |
| `autoJokes` | Enables the stage's occasional original LARICS/robot captions, default true |

`scene.mode` accepts `auto`, `orbit`, `laser`, `robots` or `quiet`. Auto changes the
stage's embellishments every 85 seconds. The service can select a mode at any time.
Energy changes the strength of the extra light effects. `palette:"ice"` changes
the main accent to cyan; other values use warm orange.

The service alternates `scene.audioStyle` between `city` and `waterfall` every
ten minutes. Palette and energy changes retain the current act's timer. Book
photos rotate independently about every 105 seconds; curated visual refreshes
follow the guests' mood and requests.

`scene.graphicsScale` defaults to 1. It scales the robot fleet and delivered
records immediately; 1.5 makes them fifty percent larger. The stage reports the
applied value as `graphicsScale` in runtime status. Photographs fit within 75% of
the free main pane's width, after excluding the right panel. They stay centered,
preserve their aspect ratio, and meet the bottom edge. The visualizer continues
behind the photo and remains visible around its edges.
Titles and captions overlay the image. Robot paths retain fixed caption clearance
behind the photo. Labels grow with the robots. The request controls,
QR and current-track panel keep their readable size.

Book images and robot paths keep the same bounds when a caption appears,
disappears or wraps onto more lines; captions never resize the photo.
Long captions fit their fixed maximum height by reducing their own font size.

The effect index controls existing qtra geometry independently of the stage mode:

| Index | Rendering |
|---:|---|
| 0 | Audio-driven cube |
| 1 | Orbiting sphere |
| 2 | Möbius ribbon |
| 3 | Sphere and ribbon |
| 4 | Wireframe cube |
| 5 | Sphere and wireframe |
| 6 | Ribbon and wireframe |
| 7 | Sphere, ribbon and wireframe |

The three bits select `actionOrbitSphere`, `actionMobiusRing`, and
`actionWireframe`. The scene modules queue their GL changes on the render executor.
Audio capture stays attached during transitions. Missing actions are reported as
`effectError` and are retried when the modules become available.

Caption IDs are consumed once per stage lifetime. Each cue keeps at least
4.5 seconds, or `text.length / 17 + 1.8` seconds for longer text, and new cues wait
for the previous one. Fades occupy 0.6 seconds at entry and 0.65 at exit. Captions
older than three minutes are discarded when a saved state is read. Fresh track
and queue changes create robot delivery missions; dialogue stays in a fixed
rectangle with a leader to the moving vehicle.

Book images preserve their aspect ratio. Prefer the original photo assets in
`~/.local/state/larics-dj/book/cards.json` where `kind` is `photo`; a complete page
works when the typography itself is the subject. `page` is the book's printed
page number, separate from the PDF page number.

## HTTP endpoints used by the native panel

| Request | Expected response |
|---|---|
| `GET /api/search?q=...` | `{tracks:[{id,uri,title,artist,album,artUrl,durationMs}],error?}` |
| `POST /api/request` with `{requestId,uri,requestedBy,dedication}` | Request status, optionally inside `request` |
| `GET /api/chat?after=...` | `{messages:[{id,role,name,text,createdAt}],dj:{status,lastSeen,processingMessageIds}}` |
| `POST /api/message` with `{guestId,name,text,requestId}` | `{message:{id,role,name,text,createdAt}}` |

## Verification

```sh
python3 -m unittest discover -s tests -p test_party_stage.py -v
```

The integration test owns an Xvfb display, qtra process, and HTTP fixture. It
exercises atomic state replacement, caption dwell and deduplication, recovery
from invalid JSON, real named-action changes, native search and request widgets,
multiline chat and dedications, query restoration, failed-send retries, stale
search replies, fixed Magic box bounds, real Enter/Shift+Enter/Escape keyboard behavior,
incoming replies, and complete stage release. The keyboard checks use `xdotool`
on the test's private X display. Set
`PARTY_STAGE_CAPTURE` to an absolute PNG path to inspect the actual window.

## Source context

The fleet colours, rotor motion, wheel distance, cargo swing, and separation of
motion from dialogue come from the Spomenar viewer's
`views/pdf-viewer/src/common/prefetch-robots.ts`. EagleRed, VIV, Navines, and MORUS
retain their distinct silhouettes; MORUS has about three times EagleRed's rotor
span in the show.

The caption timing draws on `snippets/soliloquy_intermission.cpp` and the original
Claude dialogue in session `7041d14e-f0f1-4ff8-8ea6-47f6e1c84d75`, including its
dramaturg review `agent-a39663083c88bd523`. The recovered direction favours sustained
comic acts, readable payoff lines, changing motion, and smooth handovers.

## Karaoke

`state.karaoke` is either `null` or an enabled cue sheet with `title`, `source`,
`status`, `positionMs`, `sampledAt`, `durationMs`, `offsetMs`, and `lines`. Each
line supplies `text`, `startMs`, `endMs`, and ordered `syllables` with the same
timing fields and text fragments. Spaces in those fragments are preserved.

The native stage adds elapsed time only while `status` is `playing`. It keeps the
signed `positionMs` anchor until after interpolation, then clamps to the song's
duration. The service has already applied `offsetMs`. Paused and waiting clocks
hold still; seeks select the corresponding line immediately.

A fixed lyric panel shows the active and next lines. The current syllable changes
color while a musical note travels across it and bounces. Text keeps the same
layout during highlighting; gaps hold the note and show a count-in for the next
cue. Long lines wrap and fit inside their allocated area. The karaoke panel sits
between the header and subtitle band, leaving the guest desk and photo bounds
unchanged. Native status includes its current line, syllable, clock, and geometry.

The private-display test uses the original text “Roboti pjevaju.” to exercise
pause, interpolation, negative anchors, gaps, seeks and stable lyric bounds.
`PARTY_KARAOKE_CAPTURE=/absolute/path.png` saves its actual rendered frame.
