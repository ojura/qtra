# Spotify transport and playback observations

The request service uses the existing native Spotify debugging connection when
available, otherwise the signed-in Spotify web controller. Search uses the existing
catalogue path. The reorder operations below change only the order of already
queued occurrences; enqueue confirmation binds a newly observed occurrence.

Ordinary client operations reuse an existing native debugging connection or an
already listed Spotify browser page. If neither is available, they report a
disconnected/preparation-required error and do not create a tab or prepare an
absent target. A later listed page can be attached normally. Browser/controller
setup is explicit: `python3 tools/party_dj/spotify_reconnect.py ensure` owns any
needed controller-tab creation. The native-port path is unchanged.

## Playback ownership

Spotify is the primary player for the party. Do not open, start or leave a
YouTube/provider video playing in the background. An external karaoke cue needs
explicit coordination with the live DJ, a single owner for that player, and a
verified pause of Spotify before provider audio starts. The provider must be
stopped and verified silent before Spotify resumes. Preparation or a search
result alone is not a playback cue. Keep the existing microphone, output device,
routing and volume settings intact.

## Pause and resume through Spotify Connect

The current web `PlayerAPI.pause()` and `resume()` wrappers emit cancellable page
events before dispatch. Their completion alone is not playback confirmation.
The bridge resolves the same player's Harmony controller, reads its active
Connect device, and passes that device ID to `pause` or `resume` with the existing
`forceRemoteAction` option. The controller keeps its normal authenticated command
and restriction handling. No token or cookie is extracted.

Exactly one active device must be controllable and observable. The bridge refuses
synthetic local state because Harmony can otherwise claim that state by
transferring playback to its local streamer. Browser status reads also reject
that state: the service clears the observation and reports its existing reconnect
state instead of presenting an inactive local streamer as the current player.
The bridge also refuses ambiguous devices,
an explicit rejected command, or a device change during confirmation. It sends
one command and never retries it against a newly selected device.
It checks the observed device and rereads the active target immediately before
dispatch, so a device change during the initial status read sends no command.

After dispatch, the bridge checks actual Harmony playback state for up to eight
seconds. A pause requires `paused:true`; resume requires `paused:false`; buffering
must have finished. An acknowledgement without matching state is reported as an
unconfirmed operation. A successful result includes the target `device` and the
fresh `status` observation. The native debugging client retains its direct
PlayerAPI path when Harmony is absent and confirms the returned player state.

`next` retains its existing skip path. Transport handling does not transfer
playback, change volume, or touch audio hardware.

### Explicit playback state with caller preconditions

`setPaused(paused, expectedCurrentUid, expectedDeviceId)` sets a requested pause
state for exactly the observed occurrence and device. Python exposes
`set_paused(paused, expected_current_uid, expected_device_id)` under the shared
client lock. All three arguments are required: `paused` must be a boolean and
both identity fields must be nonblank strings.

Admission requires actual Harmony state with an explicit pause boolean, a valid
clock and no buffering. The active device must be uniquely controllable and
observable, and its ID must match both the caller and the status observation.
The bridge resamples status and checks the current internal queue occurrence
synchronously after its final device read. This catches a track change while
that device read was pending. Missing internal occurrence access rejects the
operation before dispatch.

If the latest actual pause state already matches, the same preconditions still
apply and the operation returns a fresh no-op. Otherwise it sends one targeted
`pause` or `resume` and observes the same occurrence/device for up to eight
seconds. Transient buffering may clear during confirmation; success requires
the requested actual pause state, a valid clock and buffering finished.

Success returns `{ok:true, action:'setPaused', paused, changed, device, status}`.
`changed:false` means no command was needed. Errors carry `dispatched:false`
before command admission or `dispatched:true` once the command may have been
sent, including a lost acknowledgement or an unconfirmed timeout. The Python
wrapper preserves this distinction and never retries. The method has no toggle
or untargeted fallback; a new action must use a new observation and decision.

### The desktop client without Harmony

The native desktop client's page (`xpui.app.spotify.com`) has no Harmony
controller. Its PlayerAPI `pause`, `resume`, `skipToNext` and `skipToPrevious`
drive the client's own player, so `setPaused` and `skip` use them only while that
local player is the active device: `ConnectDevicesAPI.getActiveDevice()` must
report `isLocal` and the caller's device ID. Status there comes from PlayerAPI
(`playbackState.source` is `player-api`). The final synchronous occurrence guard
reads `PlayerAPI.getState().item`, and the player's own restrictions apply
(`canPause`, `canResume`, `canSkipNext`, and for previous `canSkipPrevious` or
`canSeek`). Admission, the single dispatch, `dispatched` and the confirmation by
actual state are the same as on the Harmony path. PlayerAPI first emits its
cancellable UI event; a cancelled command is reported as unconfirmed. The web
player still refuses transport without Harmony.

## Reordering the explicit queue atomically

`reorderQueue(orderedQueuedUids, expectedQueuedUids, expectedCurrentUid,
expectedDeviceId)` applies the complete desired order in one revision-checked
command. Python exposes `reorder_queue(ordered_queued_uids, expected_queued_uids,
expected_current_uid, expected_device_id)` through the same client lock used by
status, search and queue writes.

All four arguments are required. The two lists must contain unique, nonempty
UIDs and be exact permutations of each other; no occurrence is added, removed or
matched by URI. The expected list must equal the entire freshly observed explicit
queue. The current occurrence UID and active controllable/observable device ID
must match the supplied expectations before the operation proceeds. Empty,
single-item and already matching queues return a guarded no-op with fresh state.
The no-op resamples playback and revalidates the device, current occurrence,
revision and full order immediately before returning, so a pause during preflight
is published without sending a queue command.

The shared queue implementation replaces only visible explicit-queue slots with
the existing objects in their requested order. Hidden items, context entries,
repeated context UIDs, delimiters and their positions are retained along with
the current occurrence and playback history. It never reconstructs the full
queue from the public context preview. The final current/device/revision/order
checks, one targeted manager dispatch and observed confirmation are the same
ones used for a single move below.

Success returns `{ok: true, action: 'reorderQueue', moved, orderedQueuedUids,
device, status}`. Errors carry the same `dispatched` flag as a move. A stale
precondition, manual queue change or unconfirmed result requires a new
observation and a new decision; neither wrapper retries the operation.

The authenticated service endpoint is `POST /api/admin/reorder`, with the four
preconditions in its JSON body:

```json
{"orderedQueuedUids":["q-three","q-one","q-two"],"expectedQueuedUids":["q-one","q-two","q-three"],"expectedCurrentUid":"q-current","expectedDeviceId":"active-device-id"}
```

The reusable desk obtains a fresh observation and can save a reviewable plan:

```sh
python3 tools/party_dj/desk.py front q-three --save queue-plan.json
python3 tools/party_dj/desk.py apply queue-plan.json
```

Use actual occurrence UIDs from `desk.py queue`; `order` accepts the complete
desired list while `front` preserves the relative order of the remaining items.
Applying a saved plan sends its original preconditions once. If another person
has changed the queue, current occurrence or device, inspect the new observation
and make a new plan. A failed or lost response never causes an automatic replay.

## Moving an existing queue occurrence

`moveQueue(uid, beforeUid, expectedQueuedUids)` moves one explicit queue occurrence
immediately before another. The Python client exposes the same operation as
`move_queue(uid, before_uid, expected_queued_uids)` under its existing shared lock.
This compatible three-argument operation derives a desired order and uses the
same guarded implementation as the complete reorder. It captures current/device
expectations from its initial observation; new complete-reorder callers must
supply those preconditions explicitly.
Callers supply the complete ordered list of explicit queue UIDs from a fresh
observation. Both selected UIDs must occur exactly once; repeated recordings with
different UIDs remain separate requests. Missing or ambiguous occurrences,
URI-only identities and stale expected order are rejected before dispatch.

The inspected UI API is `PlayerAPI.reorderQueue(items, {before: anchor})`, but its
current implementation drops the promise returned by its queue manager. The
bridge instead awaits that same manager's
`setInternalPlayerQueue({revision, next_items, previous_items}, deviceId,
{ignoreRevision: false})`. It supplies the explicitly observed active device and
retains the normal revision check. If this API is unavailable, the operation
fails without an untargeted fallback.

The move reuses the full internal queue objects and changes only their ordering.
Current playback, previous items, context tracks, repeated context UIDs and
delimiters are retained. The public status contains only a short context preview
and is never used to reconstruct the internal queue. Immediately before sending,
the bridge rereads the current occurrence, active controllable/observable device,
queue revision and complete internal order. Synthetic state, restrictions or a
concurrent change cancel the dispatch.

Exactly one targeted command is sent. Its acknowledgement is followed by up to
eight seconds of observation of the desired explicit UID order and the complete
internal sequence, with the current occurrence, history and device unchanged.
Success returns `{ok: true, action: 'moveQueue', moved, device, status}` with fresh
state for backend publication. An already satisfied order returns `moved: false`
without a command. An error carries `dispatched: false` before a command and
`dispatched: true` after dispatch may have occurred; the Python wrapper preserves
that flag in `SpotifyError`. Bootstrap or argument-encoding failure is definitely
undispatched; a lost response after method evaluation remains uncertain. A timeout
or concurrent change is not retried.

The authenticated service endpoint is `POST /api/admin/move`; `djctl move`
accepts the same JSON body through `--json` or `--file`:

```json
{"uid":"q-new","beforeUid":"q-anchor","expectedQueuedUids":["q-anchor","q-middle","q-new"]}
```

Replace these example UIDs with the complete explicit order in fresh state, then
pass the body to `python3 tools/party_dj/djctl.py move --file move.json`.
The service reconciles the confirmed `result.status` and publishes public state
before responding. Its Spotify-error response includes `dispatched`, including
an unconfirmed command; callers must inspect fresh state instead of replaying it.

## Confirming a new queue addition

`enqueue(uri, operationId)`, exposed by Python as
`SpotifyClient.enqueue(uri, request_id)`, retains the operation ID and its
cached outcome. The request service normally uses the saved request UUID as this
ID. The Python client keeps queue writes serialized through acknowledgement and
confirmation.

The bridge awaits the existing queue manager's
`addToQueue([uri], deviceId)`. The public `PlayerAPI.addToQueue` wrapper delegates
to an async queue wrapper that drops the manager's promise and omits its device.
That omission lets Harmony select its local streamer path. Supplying the observed
standalone device and awaiting the manager preserves the actual command result.

Before dispatch, the bridge requires actual Harmony state, a known current
occurrence UID, a complete explicit queue, and one active device that is both
controllable and observable. A browser controller must target a standalone
device. It compares the actual current occurrence and observed device, rereads
the active target, then synchronously checks the internal current occurrence,
explicit queue order and add restrictions immediately before the manager call.
Synthetic state, a changed observation, missing targeted access or a restriction
returns `dispatched:false`; the public wrapper is not a fallback.

After the one targeted call completes, confirmation continues on the same actual
device. Success requires either an increased count of the requested recording
with exactly one new queued UID, or a different known current UID for that
recording which was not already in the initial queue. The recording that was
already playing cannot confirm its own new request. An ambiguous pair of new
UIDs remains unconfirmed. Success returns `{queued:true, uri, requestId, uid}`.

An explicit false acknowledgement, rejected promise, lost response, malformed
observation, device change or confirmation timeout carries `dispatched:true` and
retains the operation's cached outcome. The Python wrapper also treats a
malformed success object or missing occurrence UID as uncertain. These outcomes
require fresh observation and an operator decision before another command.

### Deliberately recovering an absent uncertain request

Keep the original saved request and cached operation outcome intact. Repeating
the same operation ID returns its cached result. Creating another service request
for the same recording can also return the existing request through URI
deduplication.

The sole queue operator can assess a new addition using this sequence:

1. Read fresh actual current-track/device state, the complete internal upcoming
   queue and all retained previous items, together with retained service track
   observations. Inspect the exact requested URI. The public context preview is
   truncated; use the existing controller's `getInternalQueue()` for complete
   `next_items` and `previous_items`. A disconnected or synthetic observation
   cannot establish absence.
2. If the requested occurrence is present, let ordinary observation reconcile its
   UID. If the item is absent and the operator deliberately decides to add it,
   choose one new stable operator operation ID associated with the saved request.
3. Call the existing Python enqueue API exactly once with that new operation ID.
   Leave the original UUID, database row and cached result unchanged.
4. Verify the returned UID in actual playback/queue state and verify that normal
   service observation attaches it to the original uncertain request. A lost,
   rejected or unconfirmed result returns to observation; do not blindly replay
   it or generate successive operation IDs.

From the repository root, after that assessment, the supported invocation is:

```sh
python3 - 'spotify:track:TRACK_ID' 'assessed-REQUEST_ID-1' <<'PY'
import json
import sys
from tools.party_dj.spotify_native import SpotifyClient

client = SpotifyClient()
try:
    print(json.dumps(client.enqueue(sys.argv[1], sys.argv[2]), ensure_ascii=False))
finally:
    client.close()
PY
```

Replace both placeholders with the verified URI and the operation ID chosen for
that single assessed action. This uses the maintained client and ordinary
request reconciliation; it does not require a database or cache repair.

## Playback clock

When Harmony supplies a complete current-track clock, its `position` and
`timestamp` take precedence over the page's normalized or optimistic fields.
Positions and timestamps are milliseconds. A paused position holds, including a
valid zero at the beginning. Elapsed time is added only while playing and not
buffering. A missing duration does not force an otherwise valid position to zero.
The raw current track also supplies identity when the page still shows an older
track. Metadata can be reused for the same URI; fallback clock fields also
require matching queue occurrence UIDs when both are known. Repeating the same
recording therefore cannot reuse its previous occurrence's clock.

`status.playbackState` exposes the clock source, separate `paused` and `buffering`
flags, the unextrapolated `positionMs`, `sourceTimestamp`, and current device
identity. `observedAt` records the fresh read time. A paused source timestamp can
remain unchanged while later reads correctly report the same held position.
`nowPlaying` also includes separate pause/buffering flags and the calculated
`progressMs` consumed by the stage.

Both `nowPlaying.clockValid` and `playbackState.clockValid` identify a real clock
anchor. A finite nonnegative position is required, including an actual zero;
playing also requires a finite positive source timestamp no more than 1,000
milliseconds ahead of `observedAt`. Explicitly paused or
buffering positions can be valid without a timestamp because they are held rather
than extrapolated. The compatibility defaults of position zero and timestamp now
never establish validity. Consumers hold when `clockValid` is false. The bridge
does not impose a freshness deadline: karaoke derives its deadline from the actual
Spotify `observedAt` plus 10,000 milliseconds.

The party service normally refreshes its public state every 2.5 seconds. A
verified transport response applies its fresh `status` through the same request
reconciliation and publishes `/api/state` and `state.json` before returning.
Polls started before that command cannot replace the newer observation. A `next`
acknowledgement without embedded status triggers one fresh read and retains the
command acknowledgement as its response.

## Verification and deployment

```sh
python3 -m unittest discover -s tools/party_dj -p test_spotify_bridge.py -v
PYTHONPATH=build/party-deps python3 -m unittest discover -s tools/party_dj -p test_spotify_native.py -v
node --check tools/party_dj/spotify_bridge.js
```

Tests execute the real bridge in Node with isolated PlayerAPI and Connect
fixtures. They cover exact target dispatch, held and zero positions, buffering,
false acknowledgements, device changes, the synthetic-state transfer guard,
native fallback and retained queue request IDs. Explicit pause-state fixtures
cover caller identity checks, missing pause flags, invalid/future clocks,
idempotent fresh no-ops, current/device races, single dispatch and unconfirmed
results. Desktop-client fixtures, on the actual `xpui.app.spotify.com` host,
cover local-device pause/resume, next, previous and restart, remote or
mismatched devices, occurrence changes, player restrictions and a cancelled
command. Queue-move fixtures additionally
cover repeated recordings, UID/anchor validation, atomic permutations, required
current/device preconditions, stale revisions/order, concurrent changes,
full context/history retention, no-ops and unconfirmed commands. Python fixtures
also check argument/error propagation and serialization through confirmation.
No live Spotify control is sent.

`SpotifyClient.call` rereads the bridge file on every invocation. Updating that
file activates the next bridge version on the following call; coordinate its
replacement with the live DJ. A candidate can be checked with
`SPOTIFY_BRIDGE_PATH=/path/to/candidate.js` before replacing the active file.
