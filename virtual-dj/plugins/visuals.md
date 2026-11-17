# Native visual seed

These modules are starting material for another DJ, another renderer or a new
act. Keep, replace or extend the parts that fit the room. The existing title
heuristics and party props are defaults; they do not prescribe a music catalogue
or require every show to use robots, photographs or cocktail glasses.

## Existing pieces

| Piece | Responsibility | Reuse seam |
|---|---|---|
| `snippets/party_dance.cpp` | Disco, streamers, conga and stadium wave; large held props | GUI module requests, state file and `audio.beat` events |
| `snippets/party_stage.cpp` | Track display, captions, robot fleet, optional photos, karaoke and guest desk | Existing state contracts and named controls |
| `snippets/audio_pulse.cpp` | Audio analysis and ring/city/waterfall renderer | Already-running `audio.beat` event stream |
| `tools/launch_party_stage.py` | Explicit session startup and module registration | Recorded host/socket/module identities |
| `tests/test_party_dance.py` | Private renderer fixture and synthetic beat source | Silent preview and regression entry point |

The dance layer is transparent, mouse-transparent and confined to the free main
pane. It sits below the stage, preserving the guest panel, captions and fleet.
It owns one asynchronous local socket subscription, its timers and its widget.
It consumes beat strength/count/BPM; it does not open an audio device. Releasing
this layer leaves the other modules running.

## Choose artwork for a real song

Read `/api/state` and use the actual `nowPlaying.uid`, or a known queued
occurrence's UID when preparing ahead. A Spotify URI identifies a recording;
the occurrence UID distinguishes repeated plays of that recording.

The authenticated scene operation accepts this optional field:

```json
{"songVisual":{"trackUid":"q123","theme":"compass"}}
```

With the service's existing CLI, from the repository root:

```sh
python3 tools/party_dj/djctl.py state
python3 tools/party_dj/djctl.py scene --json '{"songVisual":{"trackUid":"q123","theme":"compass"}}'
python3 tools/party_dj/djctl.py scene --json '{"songVisual":null}'
```

Replace `q123` with the observed occurrence. Supply `--url` and `--data` for the
chosen session. The last command clears the cue. Clearing restores automatic
selection; it does not advance the song or act.

| Theme | Existing large prop |
|---|---|
| `record` | Vinyl record |
| `speaker` | Two-cone speaker |
| `headphones` | Headband and ear cups |
| `microphone` | Hand microphone |
| `bottles` | Bottle with a large label |
| `cocktail` | Stemmed glass, straw and lime |
| `compass` | Compass rose and needle |
| `dandelion` | Stem, leaves and seed head |
| `embers` | Warm flame silhouette |

A cue applies only to its exact current occurrence. A mismatched UID, an absent
cue or an unsupported theme leaves automatic selection in charge. The existing
automatic seed uses title cues for drinking, cocktails, location, flowers and
ashes; other songs cycle musical props and colors. Queue edits and playback
clock updates do not alter the current song's identity.

Native status exposes `theme`, `automaticTheme`, `themeSource` (`automatic` or
`cue`), `songKey` and `songRevision`. Changing a cue on the same song leaves the
revision and ten-minute choreography anchor alone. The last accepted themes
survive unreadable state and module reload. A valid state with no current track
uses the remembered automatic theme; the cue becomes eligible again only when
its exact occurrence returns.

## Build and preview silently

Use the repository's configured build and host-matched modules. Once the normal
host/stage dependencies are built, only the dance target needs rebuilding:

```sh
cmake --build build/release --target agent_snippet_party_dance --parallel 2
PARTY_DANCE_CAPTURE=/tmp/dj-visual-preview \
  python3 -m unittest discover -s tests -p test_party_dance.py -v
convert /tmp/dj-visual-preview.disco.png -resize 1280x800 \
  /tmp/dj-visual-preview.disco-small.png
```

The test owns an Xvfb display, qtra host, HTTP fixture, temporary state file and
synthetic beat socket. It never loads the live audio analyzer or contacts a
Spotify account. Its captures are actual private native renders, including the
guest panel. The prefix produces `.disco.png`, `.bottles.png`, `.dandelion.png`,
`.streamers.png`, `.conga.png` and `.stadium.png`. Inspect the native render and
the exact smaller resample, then delete the generated preview files.

`RUNTIME_AGENT_TEST_BUILD_DIR` selects another built tree. The required host and
stage setup is described in [the maintained party-stage guide](../../docs/party-stage.md).
Omit `PARTY_DANCE_CAPTURE` when only a state/protocol change needs validation;
the same test command then runs without creating previews.

## Reuse and extend

Start with a runtime cue for an existing prop. For a new silhouette, add it to
`drawProp` and to the supported theme list, then extend the service validator and
CLI documentation together. Add a fixture that exercises its intended state
transition and inspect its private render. The gesture is anchored to the
robot's transformed hand, so it moves with the arm instead of floating nearby.

For a new act, preserve the event connection and release path. Extend the
bounded drawing function, act validation and cycle consistently. The existing
cycle is disco → streamers → conga → stadium; `act:"auto"` advances when
`scene.audioActStartedAt` changes. Explicit `act` requests select a choreography
without changing music. Per-song props remain independent of that schedule.

At the current 2560×1600 layout, the fixed 600-pixel guest panel leaves about
1954 pixels for art. Judge the free pane rather than the full screen width.
For a 15-inch laptop, prefer a few large figures, thick outlines and simple
silhouettes. Check hand-held props at 1280×800 too. Keep captions and current
track labels readable, preserve the composer's fixed bounds, and avoid new
layout movement while search or chat updates.

The baseline uses a 34 ms animation timer, stops painting when hidden, and caps
confetti at 24 pieces. Prefer reducing tiny details before adding expensive
effects. Use smooth movement and soft fades; keep full-screen flashes and blur
out of this seed. `frames`, `beats`, `audioConnected`, `stateError` and `drawMs`
make real progress and rendering cost observable. Empty status calls do not
request a repaint.

## Replace only this layer during an agreed live change

1. Read the session's `qtra-modules.json`, then verify the recorded socket with
   `AgentClient.hello`. Use its actual `modules.party_dance` ID. Read the dance
   status, `party_stage`'s `getComposerDraft:true`, the name/focus and window state.
2. Build and inspect privately. Use `tools/agentctl.py reload` with the GUI
   executor and the exact outgoing dance ID:

   ```sh
   python3 tools/agentctl.py --socket "$DJ_SOCKET" --compact reload \
     build/release/agent_snippet_party_dance.so --executor gui \
     --replace "$DJ_DANCE_MODULE" \
     --request '{"stateFile":"/path/to/session/state.json","agentSocket":"/path/to/session/agent.sock"}'
   ```

   Set the two `DJ_` variables and JSON paths from the observed session metadata.
   Do not run the session launcher for this operation: the launcher also shows
   the main window and configures other modules.
3. Require `installed:true`. Re-read the metadata and atomically replace only
   its `modules.party_dance` key with `loaded.moduleId`; preserve concurrent
   changes to other module keys. Verify the current act, effective song cue,
   draft/selection/name, panel geometry, focus and visibility are retained.
4. Read real frame/beat progress when the stage is already visible. If the guest
   has switched away, leave the foreground choice intact. No playback command or
   live screenshot is needed for this handoff.
5. Record the scoped commit and deployment. Remove private previews and other
   proved temporary files. Keep active build outputs and resident module copies:
   the host retains loaded modules until it exits.

For a new session or a deliberate whole-stage start, use the launcher described
in the maintained guide. Treat photo, caption, karaoke, audio and guest-control
contracts as separate extension points rather than coupling them to this act.
