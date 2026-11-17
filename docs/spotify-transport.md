# Spotify transport and playback observations

The request service uses the existing native Spotify debugging connection when
available, otherwise the signed-in Spotify web controller. Search and enqueue
keep their existing behavior. The additive queue move below changes only the
order of already queued occurrences.

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

## Moving an existing queue occurrence

`moveQueue(uid, beforeUid, expectedQueuedUids)` moves one explicit queue occurrence
immediately before another. The Python client exposes the same operation as
`move_queue(uid, before_uid, expected_queued_uids)` under its existing shared lock.
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
that flag in `SpotifyError`. A timeout or concurrent change is not retried.

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

The party service normally refreshes its public state every 2.5 seconds. A
verified transport response applies its fresh `status` through the same request
reconciliation and publishes `/api/state` and `state.json` before returning.
Polls started before that command cannot replace the newer observation. A `next`
acknowledgement without embedded status triggers one fresh read and retains the
command acknowledgement as its response.

## Verification and deployment

```sh
python3 -m unittest discover -s tools/party_dj -p test_spotify_bridge.py -v
node --check tools/party_dj/spotify_bridge.js
```

Tests execute the real bridge in Node with isolated PlayerAPI and Connect
fixtures. They cover exact target dispatch, held and zero positions, buffering,
false acknowledgements, device changes, the synthetic-state transfer guard,
native fallback and retained queue request IDs. Queue-move fixtures additionally
cover repeated recordings, UID/anchor validation, stale revisions/order,
concurrent changes, full context/history preservation and unconfirmed commands.
No live Spotify control is sent.

`SpotifyClient.call` rereads the bridge file on every invocation. Updating that
file activates the next bridge version on the following call; coordinate its
replacement with the live DJ. A candidate can be checked with
`SPOTIFY_BRIDGE_PATH=/path/to/candidate.js` before replacing the active file.
