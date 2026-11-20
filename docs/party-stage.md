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
  agent_snippet_audio_pulse agent_snippet_party_stage agent_snippet_party_dance \
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
on Wayland or X11. The return binding and Spotify's F11 binding require native
Spotify's X11 window. The helper tracks each key independently, so an F11 conflict
does not remove an available Tab binding; both release when qtra's return action
or owner disappears.

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
shortcuts, restores the prior menu/status-bar visibility, title and window cursor,
and restores the effect actions to the states they had when the stage took control.

Party-stage requests also accept `panel:true|false` (both keep the desk open), `fullscreen:true|false`, a
`requestBaseUrl` override for the local API, an
inline `state` object, and `capture:"/absolute/path.png"`. Captures include the
native overlay and request panel. `restore:true` releases the stage. Every run
returns its current revision, scene, effect, cue, pending cue count, loaded image
status, panel state, and state-file or missing-effect errors.

`getComposerDraft:true` also returns `composerDraft`, containing the multiline
text, cursor/anchor positions, selected track, saved query, latest successfully
sent message ID, and any alternate draft saved during recall. An active descriptive
search retains its text and request UUID for an idempotent resume. Pass that object
as `composerDraft` to restore the active Magic box intent during a hot reload.
The name field remains `partyName`; the composer is `partyMessage` and exposes
Qt's `plainText` property.

The draft response also exposes `guestId`, a fresh per-editor `composerId`,
`draftRevision`, exact untrimmed `text`, and `editorMode` (`message` or `dedication`).
An operator may send a GUI request with `setSearchIntent` containing those exact
five fields plus `intent:chat|music|auto`. The result is `searchIntentResult` with
`applied` and either `effectiveIntent` or an error. Editing, restoration, recall,
send, song selection/cancellation and service changes invalidate old bindings;
reloading creates a new composer identity. A chat cue immediately hides and
invalidates automatic results; a music cue searches the current text without
selecting or submitting a song. Reading the complete draft is explicit. Draft
identity and revision remain local; text classified as music can be sent as a
normalized catalogue query (up to 160 characters) before submission. There is no
separate backend draft feed or draft record.
Composer read/intent RPCs require an already active stage and editor. A resident
but released module fails these calls before construction; install/configure the
stage in a separate request first. This closes a stale module-metadata race at the
GUI invocation itself.

Each dispatched catalogue request carries a fresh opaque UUID as `searchId` on
`GET /api/search`. Invalidating that request aborts its reply and sends
`POST /api/search/cancel {searchId}` to the service that received it. This includes
edits, intent changes, selection, send, recall, a service change and module release.
The cancellation outlives the old desk during a hot reload. Replies must match the
current request ID and revision; cancelled replies are quiet, and old replies
cannot clear a newer request's activity indicator. Sending or recalling also
removes any already displayed obsolete choices while preserving the editor draft.
A failed or mismatched catalogue response cancels the abandoned request ID before
showing its error; the client never retries that GET automatically.

## Guest controls

- **Tab** switches qtra to fullscreen native Spotify, and **Tab** in Spotify
  returns to qtra with the same draft and focus. The helper binds Tab only on
  Spotify's managed windows; other applications retain their own Tab behavior.
  Closing or relaunching Spotify refreshes those bindings. Switching windows
  leaves the track, queue and volume alone.
  Matching clickable shortcut controls sit in the main-stage header beside the
  observed playback button and directly above **Daj pjesmu**. They show **Tab**,
  **Ctrl+P**, **Ctrl+K**, **Ctrl+B**, **Ctrl+N** and **Ctrl+L**. One `guestShortcuts`
  specification supplies the visible keys, labels and QAction bindings; the
  header controls share the playback button's style and scale. The lower Magic
  box help retains its Tab reminder; the sidebar has no separate shortcut block.
- The native request and chat panel stays open. Click **Zatraži pjesmu · R**, or
  press **R** or **Ctrl+K**, to focus its bottom **Magic box**, which accepts a song, artist, or a
  message to the DJ. Typing shows debounced results above it; **Pošalji poruku (Enter)** always
  sends chat until a song is explicitly selected.
- Exact song or artist queries use the direct catalogue search after a 330ms
  typing debounce. Cheap local intent gating suppresses obvious conversation and
  UI instructions before a catalogue call; unfamiliar concise titles still use
  the direct path. **Ctrl+K** explicitly searches the current text, including text
  that the local gate treated as conversation. **Traži po opisu** explicitly asks the DJ to resolve a remembered
  description. The panel shows that work as pending and polls its result while
  the same query remains active. Editing cancels that view and immediately returns
  to the ordinary catalogue path; a late descriptive reply cannot replace it.
  Resolved candidates still require selection and Send.
  When the resolver supplies a match basis, the row labels it as title, literal
  lyric or semantic matching. Its tooltip shows the supplied rationale, optional
  confidence, and up to four HTTP(S) source labels/URLs; the selected song retains
  the same explanation and complete track data. Ordinary catalogue results do not
  acquire an inferred basis or confidence. Tooltips escape supplied text and do
  not fetch or open source links.
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
- The top-right **DJ online/offline** badge follows only the service's explicit
  `dj.llm` lease. Missing or malformed data is unavailable. The badge expires on
  a local monotonic timer even when the last JSON is unchanged; a running event
  watcher or old work mark never renews it.
  Online leases require a finite receipt/expiry horizon of 30–120 seconds
  and an integer active-session count from 1 to 256. Chat callbacks are scoped to
  the service generation, so a late response from a previous URL cannot replace
  the current conversation or presence.
  An unchanged expiry retains the earlier monotonic deadline even when agent count
  or reported remaining time changes, including across unavailable/offline replies.
- **Escape**, or the small **×** beside the latest pending own message, recalls it
  only before the DJ has accepted it for processing. A read receipt alone keeps
  recall available; an acceptance timestamp, durable work record or legacy active
  processing claim closes it. The text returns to
  the composer for editing. **Vrati skicu** restores the previous draft, including
  any selected song, dedication and text selection. If typing races the reply,
  the new draft stays in place and **Uredi povučenu** opens the recovered text.
  A failed recall preserves the draft. Escape never closes the panel or changes
  fullscreen.
  The recall icon uses the shared 34px icon size and a padded clickable span.
- Work marks are separate from the grey read check: orange **●** means active
  work, **◷** means waiting, **◇** means blocked, **?** means an active lease expired
  without confirmation, green **◆** means completed, and **–** means cancelled.
  Hover a work mark for its actual summary, outcome and evidence. `chat.work` and
  `dj.messageWork` update existing rows even when no new chat arrives. An expired
  lease never becomes a completion check. Legacy `processingMessageIds` is used
  only for messages that have no durable work record yet.
- Incoming messages scroll smoothly while the transcript is following its
  bottom. Sending a guest message follows it to the bottom as well. Typing,
  scrolling or clicking interrupts that motion. After about
  two minutes without input or a new/edited message from either the guest or DJ,
  the conversation eases back to the latest message. Ongoing typing, scrolling,
  dragging or replies restart that quiet period; receipt and heartbeat refreshes
  do not.
- The sidebar is 600 pixels wide. Suggestions float over the bottom of the
  conversation, directly above the composer, and disappear when there are no
  results. The transcript and composer keep the same bounds throughout loading,
  selection, empty results and errors. While typing, existing results stay in
  place with a quiet activity bar;
  stale choices are disabled until the new results arrive. Hover highlights and
  pixel scrolling make the list easier to scan. The popup is up to 340 pixels high,
  clamped to the available transcript. Long titles are elided with full text in
  tooltips. Scrollbars retain their thin width with a lighter, contrasting thumb.
  A blue search surface, cyan border and light selected row distinguish the popup
  from the conversation. Supplied Spotify covers appear beside the song metadata;
  a missing or failed image retains a musical placeholder. At most two image
  downloads run together, with a 24-image cache and the same URL/decode bounds as
  current-track covers. Changing the query cancels and invalidates old thumbnails.
- Send sits ten pixels above the panel bottom. Short outcome/error notices appear
  for six seconds over the transcript, without reserving an empty footer or
  moving the composer, button or results.
- One cursor scope covers the DJ window, descendants created later and its owned
  Qt tooltip/pop-up surfaces. It retains arrow, text, link and resize meanings at
  48 pixels and repairs the actual native surface after hover or enabled-state
  transitions, including when the pointer stays still. Release restores the
  original widget/native cursors; other windows and the desktop keep their own
  cursors. Private Xvfb cases inspect displayed XFixes size, alpha and hotspots
  through tooltips, outside-window movement, disabled playback and release.
- **F11** toggles qtra fullscreen. In native Spotify, plain **F11** toggles only its
  focused window; Caps/Num Lock variants work too. Modified F11 and other apps
  retain their own behavior. This does not switch to qtra or affect playback.
- The current-track card includes its supplied album cover. Up to four upcoming
  titles/artists appear in a larger fixed bottom strip. Native typography is
  centralized in `UiMetrics`: 29px body, 26px secondary, 24px small text and 34px
  icons at the target desktop size; painted elements follow the free pane's scale.
  The larger QR and request heading align with the current-track heading. Caption,
  photo-label and queue bands remain separate and fixed; guest controls do not
  move. Pending requests are labelled as waiting for the DJ.
- The visible playback button reports the observed state and accepts one explicit
  pause/resume click or **Ctrl+P**. Its observed play/pause label stays visible while
  a smaller separate line reports verification. It is disabled for a disconnected, stale, buffering or invalid
  clock, missing occurrence/device, unavailable private local control, or an
  in-flight request. The client does not retry or toggle an uncertain command.
  The current-track progress bar also holds its last accepted position on an
  invalid/future clock and caps interpolation at the ten-second observation
  deadline; an invalid compatibility value cannot reset it to zero.
- **Ctrl+N** selects the next song; **Ctrl+B** uses Spotify's previous action.
  Depending on the current position and native restrictions, previous either
  returns to the preceding occurrence or restarts the current song. The matching
  header buttons invoke the same actions. Playback, next and previous share one
  fresh-state/UID/device guard and one pending state; keys suppress autorepeat.
  An uncertain or lost response is refreshed from canonical service state and is
  never retried. A restart notice requires the returned same-URI/UID/device and
  valid nonbuffering before/after clocks to demonstrate a rewind near the start.
  Native navigation posts to `/api/admin/skip` with `direction`, the expected
  current occurrence/device, `source:native` and the existing private operator
  header. It changes the display only from the subsequent canonical observation.
- **Ctrl+L**, or the matching **Stihovi** header button, toggles the local
  lyrics/karaoke surface. The highlighted button indicates the open mode. It
  preserves the editor, selection, focus and playback; its visibility choice
  survives song changes and a stage hot reload. The mode starts closed.
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
| `nowPlaying` | `title`, `artist`, `uid`, `uri` or `id`, `playing`, `paused`, `buffering`, `clockValid`, `progressMs`, `durationMs`, `observedAt` in epoch milliseconds |
| `queue` | Tracks with `title`, `artist`, `uri` or `id`, `requestedBy` |
| `scene` | `mode`, `energy` from 0 to 1, `palette`, `effect` from 0 to 7, `graphicsScale` from 0.75 to 2 |
| `caption` | `id`, `text`, `seconds`, `createdAt` in epoch milliseconds |
| `bookMoment` | `imagePath`, `title`, printed `page`, optional `expiresAt` in epoch milliseconds |
| `bookEnabled` | Service photo switch; false clears the cue and suppresses scheduled and manual photos |
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
`bookEnabled:false` suppresses photographs for a procedural-only act. The morning
direction allows occasional curated photographs again; the runtime switch and
current audience request govern that choice rather than a fixed taste in code.

`scene.graphicsScale` defaults to 1. It scales the robot fleet and delivered
records immediately; 1.5 makes them fifty percent larger. The stage reports the
applied value as `graphicsScale` in runtime status. Photographs fit within 75% of
the free main pane's width, after excluding the right panel. They stay centered
and preserve their aspect ratio. The photo's title and book page sit on their
own dark band between the photo and the queue strip, so their contrast does not
depend on the photo's content; the photo ends above that band. The visualizer
continues behind the photo and remains visible around its edges. The fleet's
name tags grow with the robots and carry their own dark backing. The request
controls, QR and current-track panel keep their readable size.

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
| `POST /api/search/describe` with `{guestId,requestId,text}` | `{search:{id,text,status:"pending"\|"resolved",tracks:[],note}}` |
| `GET /api/search/describe?id=...` | The same descriptive-search result; resolved tracks use the catalogue format |
| `POST /api/request` with `{requestId,uri,requestedBy,dedication}` | Request status, optionally inside `request` |
| `GET /api/chat?after=...` | `{messages:[{id,role,name,text,createdAt,status,readAt,acceptedAt,work}],withdrawnMessageIds:[],dj:{status,lastSeen,processingMessageIds,messageWork}}` |
| `POST /api/message` with `{guestId,name,text,requestId,source:"native"}` | `{message:{id,role,name,text,createdAt}}` |
| `POST /api/message/recall` with `{guestId,messageId}` | `{recalled:{id,text,requestId}}`; retries of a successful recall return the same result |
| Authenticated `POST /api/admin/playback` with `{paused,expectedCurrentUid,expectedDeviceId,source:"native"}` | `{ok,action:"setPaused",paused,changed,device,status}`; failures report `dispatched` |

The native API base is supplied to the RequestDesk constructor before any initial
visibility-triggered fetch. `requestBaseUrl` overrides the state file's local URL;
private callers should provide it on their first stage run, including when testing
missing or malformed state. Descriptive jobs poll at 1.5-second intervals only
while the same editor intent is active. `acceptedAt` permanently removes the recall
affordance even after a processing lease ends. Withdrawal IDs remove existing
transcript rows; they do not discard a pending recall retry's identity.

Resolved candidates use `track.match:{basis,rationale,confidence?,sources}`. The
native reader also accepts the explicit flat `matchBasis/rationale/confidence/sources`
shape during service upgrades. A work record contains `state`, `effectiveState`,
`summary`, `outcome`, `evidence`, `updatedAt` and `activeUntil`; the UI keeps the
latest record and derives an expired active lease as `unconfirmed` locally too.

Playback authentication is read only on an explicit click from `admin-token`
beside the configured state file, and sent only to the loopback API with
`X-DJ-Operator:qtra-native`. It never appears in status or artwork requests.
After either success or uncertainty the client reads canonical published state;
raw command observations cannot replace request/dedication metadata. HTTP refreshes
older than the current file observation or revision are ignored. The server owns
the fresh occurrence/device check and single explicit transport operation.

Artwork uses `nowPlaying.artUrl` (or a supplied `image` URL), one asynchronous
request, an eight-image cache, a 2MiB response cap, a 4096-pixel source-side limit,
and a 320-pixel decoded image. Late replies from an older URL are discarded.
Painting performs no network or file work; a failed cover keeps a musical placeholder.

## Verification

```sh
python3 -m unittest discover -s tests -p test_party_stage.py -v
```

The integration test owns an Xvfb display, qtra process, and HTTP fixture, and
checks their display, PID and private service port before input. It rejects host
display mode and supplies the service URL before stage construction. It
exercises atomic state replacement, caption dwell and deduplication, recovery
from invalid JSON, real named-action changes, native search and request widgets,
multiline chat and dedications, query restoration, failed-send retries, stale
search replies, fixed Magic box bounds, real Enter/Shift+Enter/Escape keyboard behavior,
incoming replies, recall failure/racing edits/idempotent recovery, explicit
descriptive search and stale resolution, scoped cursor dimensions, compact footer
bounds, and complete stage release. The idle-return test waits for the actual
two-minute quiet interval and checks that an incoming DJ reply resets it.
The keyboard checks use `xdotool`
on the test's private X display. Set
`PARTY_STAGE_CAPTURE` to an absolute PNG path to inspect the private window; the
same run also writes `native-ux-search.png` and `native-ux-empty.png` beside it.
It also writes `native-artwork-queue.png` from the cover/control fixture. The
private tests cover stale artwork, cache/response bounds, separate durable/read
marks, explicit attributed playback, no retry after uncertain dispatch, and a
delayed canonical HTTP reply racing a newer state file.
The follow-up fixtures also read actual XFixes cursor pixels through a stationary
click/busy/disabled transition, exercise Ctrl+P/Ctrl+K, reject stale exact-draft
intent cues, expire unchanged LLM leases and cancel stale result thumbnails.
`python3 -m unittest discover -s tests -p test_window_toggle.py -v` runs the
separate private Tab/F11 lifecycle fixture.

## Procedural dance act

`snippets/party_dance.cpp` is an independent GUI module that submits its actors
to the shared scene compositor. It follows the free main pane's bounds; the
permanent guest panel and captions keep their screen-space layout. The fleet
and dancers share scene depth. The first act combines
a faceted disco ball, a perspective neon floor, slowly sweeping lights and up to
24 larger confetti pieces. The second act uses flowing ribbons and orbiting records.
The third adds six larger conga robots led by a smiling vinyl record in a party
hat; speaker bodies, antennae, arms and legs dance along a winding floor trail.
The fourth is a stadium wave with eight large speaker-robots and occasional
four-second OPA/BIS scorecards with soft fades. Fewer floor tiles and mirror-ball
facets keep the large silhouettes readable on a 15-inch laptop.

The fifth is a circular **kolo**: eight large robots share hand positions around
a closed ring, step together and rotate with depth ordering. The sixth is a blue
tram with six dancing robot passengers, a moving city backdrop, turning wheels
and the destination display **SLJEDEĆA: REFREN**. Both are procedural drawings in
the same bounded scene; they require no image downloads or extra audio source.

Actual `nowPlaying` occurrence, URI, title and artist select the song artwork.
Robots hold large bottles for titles mentioning drinking, cocktail glasses for
Koktel ljubavi, compasses for Gdje sam bio, dandelions for Maslačak, and flames
for titles mentioning ashes. Unmatched songs alternate large records, speakers,
headphones and microphones. Prop colors change with each new song; progress
updates, searches and queue edits leave the currently playing song's art alone.
The song key and revision survive a module reload. Native status exposes
`theme`, `automaticTheme`, `themeSource`, `songKey` and `songRevision` for the DJ
to coordinate captions. `themeSource` is `automatic` or `cue`.

An optional `scene.songVisual` selects an existing prop without rebuilding:
`{"trackUid":"the-current-occurrence-uid","theme":"compass"}`. The supported
themes are `record`, `speaker`, `headphones`, `microphone`, `bottles`, `cocktail`,
`compass`, `dandelion` and `embers`. The UID must match the actual current
occurrence; a prepared cue for another queued occurrence waits without changing
the current art. Clear with `null` or omit the field to restore automatic art.
Unknown themes also fall back to automatic art in the renderer. Cue changes leave
the song revision and act clock intact. An unreadable state file retains the last
accepted art, including through a module reload. A valid state with no current
track shows the remembered automatic motif until a matching occurrence returns.

The visuals are reusable starting material, with runtime selection and extension
procedures in [the visual plugin seed](../virtual-dj/plugins/visuals.md).

The launcher loads it with `stateFile` and `agentSocket`. One asynchronous local
socket subscribes to the existing `audio.beat` events; the module never opens an
audio device or controls playback. Beat strength gently lifts the geometry's
brightness, with a 0.55-second decay. The act is drawn for every frame the cube
renders, about 60 per second, and stops while the window is hidden. No
full-screen flashes or blur passes are used. Status reports frames, received
beats, connection state and paint time.

Disco, streamers, conga and stadium retain the current song's large held motif
and the party toast. Kolo keeps both hands linked; the tram shows passengers
dancing inside its windows.

`act:"auto"` advances the six forms every **60 seconds**, independently of the
service's slower background scene changes. The order is `disco`, `streamers`,
`conga`, `stadium`, `kolo`, `tram`. `act` with any of those names holds that form
until automatic mode is selected again. `startAct` selects the first form while
keeping automatic mode, for example `{"act":"auto","startAct":"kolo"}`.
Optional `formSeconds` accepts a whole number from 1 to 600 for deliberate cadence
changes. Rotation continues with an unchanged or temporarily unreadable state
file. Status exposes `availableActs`, `formation`, `formIntervalMs` and `nextActAt`.
Releasing or hot-reloading this module removes only
its scene owner, timers and event connection; the audio and request-panel modules stay
running. Use the GUI executor. The current choice survives a module reload in
the host window's properties, together with the remaining form interval.

Run `python3 -m unittest discover -s tests -p test_party_dance.py -v` for the
private-display lifecycle, synthetic audio-event and occurrence-cue regressions,
plus the new kolo/tram rendering, independent clock, manual hold and reload cases.
These tests own their display, host, HTTP fixture and synthetic beat socket.
`PARTY_DANCE_CAPTURE=/tmp/dance.png` writes private scene captures, including
`dance.kolo.png` and `dance.tram.png`, for a visual check at the target size.

## Shared scene depth

`include/demo/party_scene.h` owns one `partySceneDepth` QObject beneath the
existing CubeWidget. The separately loaded stage, dance, orbit, Möbius and audio
modules discover that same object and register explicit owners. The registry lives in the host object
tree; it is not a static per-library registry. Each owner unregisters before
destroying its drawing state. The last owner disconnects the frame hook and
releases GL resources with the correct context current. The host retains loaded
libraries, including the code belonging to the generation that created the
registry, while other owners continue to use it.

The frame hook uses the host's current projection, framebuffer and depth buffer.
Each fleet body, fleet name tag and each dancer, prop holder or scenery
contribution is its own billboard. Wheels, hands, carried props and tram
passengers stay with their body's drawing. A walking robot's tag stands on the
floor just in front of its wheels, so the floor rows in front of the robot never
cover it; a flying robot's tag hangs below it at its depth. A shared ground-plane depth function relates projected foot
positions to camera distance. Floors and shadows use that ground plane, while
ambient marks occupy a far plane. Opaque pixels participate in depth testing
and writes; transparent padding is discarded, and translucent edges and marks
blend without writing depth. The host cube draws first. The shared scene then
runs each geometry provider's preparation once, its optional background once,
and all opaque sphere/rim work before the actors' opaque pixels.

Transparent Möbius interiors and audio ring triangles/points use the same
camera-distance intervals as the actors. For an interval from far F to near N,
the half-open rule is N < distance <= F. At each viewport row the common ground
depth splits that interval: far legacy material, the complete stable ground
group, then near legacy material. Flat actor groups follow in far-to-near
order, with no extra interval between peers at equal depth. Fragment distance
comes from the actual projection, viewport origin/height and depth range.
Ground meshes split at reciprocal-depth clamp boundaries so their rasterized
depth matches that row calculation. This lets partially transparent actors
cross the actual translucent rings as well as opaque cube/sphere/rim geometry.
Providers only draw their prepared buffers during slices; analysis, history,
animation clocks and beat events advance once per frame.

Delivery speech bubbles, photographs, lyrics, captions and request controls
remain screen UI above the scene. The stage draws its state-driven UI (header,
current track, request card and QR, photograph and queue) into a cached layer
once per state change. A repaint copies the changed part of that layer and draws
only the time-driven parts on top: the track's progress line when its filled
width changes, caption fades, the delivery bubble while its pointer follows the
robot, and lyrics. The delivery bubble belongs to the scene, so it is hidden
while a photograph is shown. Status reports `overlayPaintMs` and
`overlayLayerPaints`.

The compositor records and draws every actor for each frame the cube renders,
so actors move at the cube's frame rate, about 60 per second. Each actor's
QPainter commands are recorded in memory by a paint engine that also measures
their device bounds, text included. Qt's OpenGL paint engine replays the
recordings into a window-sized, 4x multisampled page; the page is resolved into
one atlas texture holding up to four pages. Nothing is rasterized on the CPU or
uploaded. A shared sampling scale shrinks only when the actors would need more
than four pages, so large viewports keep every submitted actor. At most 96
actors are accepted per frame. A ground sprite drawn for one depth interval
covers only the screen rows whose floor depth lies in that interval.

Qt's paint engine sets the GL state it uses and assumes defaults for the rest.
The compositor therefore disables face culling (the cube enables it, and the
flipped page reverses the winding of every glyph quad), sampler objects and the
pixel-unpack buffer before painting a page. It binds the page through
QOpenGLFramebufferObject, which records it as the context's current
framebuffer: Qt's glyph cache copies through a framebuffer of its own and then
rebinds that recorded one. GL program/buffer/texture, sampler, blending,
depth/range, stencil, viewport, scissor, framebuffer, clear-value, point-size,
rasterization and pixel-unpack state are restored for neighboring render
callbacks. Stage status exposes `sceneDepth` with owner and frame counts, actor
bounds/depth, atlas pages, raster pixels and scale, per-frame `timingMs` for
recording, page painting and compositing, transparent draw counts and errors. Audio reports `framesPrepared`, `backgroundDraws` and
`sliceDraws`. Entry points reject a wrong QObject thread before touching their
owner; releases that need GL also require the cube's own current context.

### Fleet marks

`include/demo/party_formation.h` owns a second shared object, `partyFormations`.
An act publishes marks: floor points where visiting walkers stand, floor points
under visiting flyers with their height, and a floor lane for performers who
cross the set. The dance module provides the marks for the current act, computed
from the same set geometry its drawing uses. The stage's fleet reads them every
frame: EagleRed takes the first flyer mark, VIV and Navines the walker marks,
and the delivery robot drives or flies along the lane. Each robot's depth comes
from its floor point, and a flyer casts a floor shadow there. Without a
provider the fleet keeps its original spread across the stage.

| Act | Walkers | Flyer | Lane |
| --- | --- | --- | --- |
| disco | dance on the floor in front of the fans | circles the disco ball, passing in front of and behind it | in front of the fans |
| streamers | dance between the fans | weaves along the ribbons | in front of the fans |
| conga | join the end of the line | leads the line with a record | in front of the line |
| stadium | stand in the aisle between the rows | sweeps above the crowd | the aisle |
| kolo | turn a small ring inside the kolo, against its direction | circles above the dancers with a record | in front of the ring |
| tram | wait on the platform in front of the tram | patrols above the roof, in front of the wire | the platform |

The focused private checks are:

```sh
python3 -m unittest discover -s tests -p test_party_stage.py -k shared_scene_crosses -v
python3 -m unittest discover -s tests -p test_party_stage.py -k shared_scene_transparency -v
python3 -m unittest discover -s tests -p test_party_stage.py -k book_caption_band -v
python3 -m unittest discover -s tests -p test_party_dance.py -k shared_scene_budget -v
python3 -m unittest discover -s tests -p test_party_dance.py -k shared_scene_rejects -v
python3 -m unittest discover -s tests -p test_party_dance.py -k acts_place -v
```

They draw a controlled real GL plane in front of and behind the fleet, check
transparent padding and neighboring GL state, retain the editor and lyric mode,
exercise 4K streamers within four atlas pages, and release/reload distinct
module generations in both acquisition orders. The marks case checks that the
tram's walkers stand nearer than the tram and below its wheels, that its flyer
stays above the roof, and that the kolo's inner pair stays inside the ring and
moves. The caption case shows a photograph with a white lower edge and checks
that the photo ends above its caption band, that the band is dark beside its
text, and that an unchanged state does not redraw the overlay layer. The transparency case uses the
real Möbius/orbit providers, partial-alpha peers, tilted ground and reversed
provider replacement order. Its audio child preloads a bounded synthetic Pulse
reader that accepts only the fixture's device token, with an unreachable Pulse
server and autospawn disabled; it opens no desktop monitor. The affinity case
rejects off-thread release, keeps owners intact and then succeeds on GUI retry.
`PARTY_DEPTH_CAPTURE=/tmp/depth.png` saves these fixtures' useful crossing views.
All inputs, rendering and synthetic events stay in the fixture-owned display
and host.

For a live replacement, load unique resident library paths and snapshot the
current editor/window state plus each enabled geometry action/configuration.
Release old owners with their required GUI/render executor, install the new
owners and rebind even disabled actions without enabling them. Audio requires
its actual capture device/options/events/style/stateFile; its normal replacement
rebuilds FFT/visual history while retaining that configuration. After a completed
QAction handover, rollback loads the preserved prior binary at a fresh path so
its new module statics can reconnect the old action. A retained module ID whose
action pointer already exists is insufficient for that handover. Update only
the replaced metadata keys after verifying the new owners and preserved state.

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

**Ctrl+L** opens the fixed lyric region for the actual current song. With no
matching authored sheet, the native stage reads
`GET /api/admin/lyrics?requestId=...&expectedCurrentUid=...&expectedUri=...&expectedDeviceId=...`
on demand. These are the only four query fields. The actual URI, occurrence UID
and active device must all match; a repeat of the same recording is a new target.
The request uses the local private token and `X-DJ-Operator:qtra-native`, without
changing playback, the queue, speech or the authored karaoke state.

The response supplies `availability:ready|unavailable|instrumental`,
`timing:line|none`, provider attribution, and `{text,startMs}` lines. Line timing
highlights the complete current line and shows the next source line. It preserves
the supplied line starts without inventing word, syllable or end timestamps.
Untimed material stays as literal, scrollable text in the same panel. Provider
attribution remains visible; an instrumental label requires the source's explicit
result. Loading, unavailable and error states use the same fixed area.

A request generation is invalidated by closing the mode, changing its service,
state file or current UID/URI/device, or releasing the stage. Late replies must
match both that generation and the exact echoed target. Clock-only observations
do not refetch. An unsuccessful lookup stays quiet until a new target or an
explicit close/reopen; it does not retry playback or another control. Reads are
bounded to 2MiB of JSON and the service's 1,000 lines / 100,000 text characters.
`sourceLyrics` runtime status exposes availability, target, timing, provider,
clock and static-scroll state without exposing credentials.

`state.karaoke` remains the independent authored rehearsal surface. It is either
`null` or an enabled cue sheet with `title`, `source`, `trackUid`, `trackUri`,
`status`, `positionMs`, `sampledAt`, `durationMs`, `offsetMs`, `clockState`,
`clockValidUntil`, and `lines`. Each line supplies `text`, `startMs`, `endMs`, and
ordered `syllables` with the same timing fields and text fragments. Spaces in
those fragments are preserved. A valid authored sheet takes precedence over the
on-demand reader.

For an authored Spotify sheet, a nonempty `trackUid` must exactly match the
current occurrence. The UID remains authoritative across URI relinks. A sheet
waiting for its first occurrence binding, a sheet for another occurrence, and an
explicitly unmatched or disconnected sheet expose no lyric text. Manual sheets
are explicitly headed **PROBA STIHOVA** and use their own title and clock,
independently of Spotify. They are intended for supplied-text rehearsal.

Runtime configuration `lyricsMode:true|false` provides the same local visibility
control as the shared action. `karaoke.mode`, `panelVisible`, `available`, `manual`
and `reason` distinguish the open UI, matching material and current availability;
`active` means that both the mode and its available lyrics are on. The mode
preserves the editor, focus, window state and all existing stage geometry.

The native stage adds elapsed time only while a trusted clock is running.
Current-source line selection uses the same actual track clock as the progress
display, preserving its last accepted position on invalid data and capping
interpolation at the ten-second observation deadline. Pause and buffering hold
the clock. Untimed text never acquires synthetic timing.

For authored Spotify cues, `clockValidUntil` is the actual observation time plus
10,000ms; interpolation stops exactly at that deadline and the status becomes
`held`. `clockState` must be `running`, and `nowPlaying.clockValid:false`, pause
or buffering also holds the clock. A frozen stale sample never continues
advancing or resets to its original anchor. Manual cues use their explicit clock
without a Spotify lease. The signed `positionMs` anchor is retained until after
interpolation, then clamped to the song's duration. The service has already
applied `offsetMs`. Paused and waiting clocks hold still; seeks select the
corresponding line immediately.

The authored syllable renderer changes the current syllable's color while a
musical note travels across it and bounces. Text keeps the same layout during
highlighting; gaps hold the note and show a count-in for the next cue. Long lines
wrap and fit inside their allocated area. The lyric panel sits between the header
and subtitle band, leaving the guest desk and photo bounds unchanged.

The private-display fixtures own their HTTP server, Xvfb and qtra process, and
use original text. They exercise source identity and generation races, repeated
songs, changed services, close/reopen/release, malformed or oversized replies,
literal scrollable untimed text, attribution, actual clock holds, manual-sheet
precedence and unchanged editor/focus. No live provider or player is used.
The authored “Roboti pjevaju.” fixture retains pause, negative-anchor, gap,
seek, syllable-note and exact-deadline coverage.

`PARTY_SOURCE_LYRICS_CAPTURE=/tmp/source-lyrics.png` writes
`native-source-lyrics-line.png` and `native-source-lyrics-static.png` from the
same private source fixture. `PARTY_KARAOKE_CAPTURE=/absolute/path.png` saves the
authored frame. `PARTY_LYRICS_CAPTURE=/absolute/path.png` saves the Ctrl+L fixture
and its unavailable view.
