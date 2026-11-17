# Virtual DJ

Use the existing Spotify player, keep a short intentional queue ahead of it, and
let the room determine the next move. This directory gathers the working desk,
practiced procedures, silent rehearsals and creative material from the LARICS
night. Its songs, robots and jokes are starting points for a future set; they do
not define the audience's taste.

The native Spotify application produces the music. qtra renders the stage and
request desk. The local party service observes Spotify, stores requests and
forwards deliberate queue or transport commands. An existing signed-in browser
controller can provide Spotify Connect access when native debugging is absent.

## Open the desk

Run commands from the qtra repository root:

```sh
python3 tools/party_dj/launch.py status
python3 virtual-dj/dj.py status
python3 virtual-dj/dj.py queue
python3 virtual-dj/dj.py inbox
```

`status` requests a fresh Spotify observation. `status --cached` inspects only
the service's published state. `queue` returns every observed explicit queue
occurrence with its UID, plus context tracks and separately identified pending
requests. A saved request is not yet a confirmed Spotify queue entry.

Queue planning requires a complete explicit queue, an identified current
occurrence and active device, and an observation no older than ten seconds.
Disconnected or invalid observations stop the operation. Ordinary observations
do not create a controller tab. Explicit connection setup and recovery are
documented in [Spotify transport](../docs/spotify-transport.md).

For a new session, start or reuse the service and native show:

```sh
python3 tools/party_dj/launch.py start --fullscreen
```

Use the returned guest URL or the stage's QR code on the same reachable network.
The laptop's request box searches Spotify directly. Selecting a result retains
the song while the same box accepts an optional dedication; Send submits it.
The separate **Traži po opisu** action asks the live DJ to interpret a description.
Exact searches have no reasoning step. Escape or the small × recalls the latest
own message only before the DJ has accepted it, preserving another draft.

## Keep the next two or three songs intentional

1. Read the current song, real explicit queue and recent history together.
2. Read the room: dancing, singing, conversation, requests and departures. Use
   [the playbook](PLAYBOOK.md) for a fast response, then search actual candidates.
3. Maintain two or three fitting songs at the front. Preserve the current song
   and the rest of the queue while changing upcoming order.
4. Reobserve after a queue change and at each transition. Keep an escape route
   for a new audience signal instead of programming a long inflexible set.

```sh
python3 virtual-dj/dj.py history
python3 virtual-dj/dj.py directions
python3 virtual-dj/dj.py search 'Dino Dvornik'
python3 virtual-dj/dj.py front 'ACTUAL_QUEUE_UID_1' 'ACTUAL_QUEUE_UID_2'
```

`front` previews the full requested order; it writes nothing by default. Select
UIDs from the current `queue` result. The chosen occurrences move to the front
in the supplied order, and every other explicit occurrence keeps its order.
`order` accepts a complete permutation when the whole order needs attention.

```sh
python3 virtual-dj/dj.py front 'ACTUAL_QUEUE_UID_1' --save /tmp/dj-front.json
python3 virtual-dj/dj.py apply /tmp/dj-front.json
```

The bridge compares the full queue, current occurrence and device immediately
before one revision-checked write. A changed observation rejects the plan; read
the new queue and decide again. A lost write response remains uncertain and is
not replayed automatically. The return includes Spotify's observed result.

To add a searched recording, use its exact URI and a stable request ID:

```sh
python3 virtual-dj/dj.py add 'spotify:track:SEARCH_RESULT_ID' --request-id 'set-next-001'
```

`--front` waits briefly for that request's actual Spotify UID and promotes it
only if the observed queue, current occurrence and device stayed compatible.
Any observed intervening manual change is remembered even if the state later
reverts. A newly added occurrence must appear in its expected appended position
before automatic promotion; an observed manual placement is left for review.
`frontPending` means the saved request needs inspection, not resubmission.
The command prints its request ID before submitting, so a response loss does
not lose the identity of the intended request.

The desk checks observed history by URI and normalized artist/title, including
common remaster and version suffixes. An intentional repeat uses `--allow-repeat`.
This catches familiar alternate recordings without treating the seed catalogue
as a ban on music the room wants.

## Stay present and respond quickly

One operator owns the event cursor and guest replies:

```sh
python3 virtual-dj/dj.py --operator 'night-dj' watch --cursor /tmp/dj-events.json
```

The first read starts at the present. Subsequent reads resume the saved cursor,
including across a restart. Output is delivered before that cursor advances.
Temporary read failures retry with the same cursor; malformed data stops with
an error. Recalled messages are filtered from both concise and `--raw` output.
`--raw` includes the full event payload and published state. Use `queue` whenever
an operation needs the fresh full queue rather than the watcher's short preview.
Concise state shows work counts; changed work records remain in their events,
and `inbox`, `message-info` or raw state provide the details without repeating
every historical outcome on every quiet poll.

Song requests, mood changes and descriptions deserve a quick acknowledgement
followed by the action they actually need. Send a reply using a stable request ID
and the last message actually read:

```sh
python3 tools/party_dj/djctl.py reply --json '{"requestId":"dj-reply-001","readThrough":123,"text":"Vidim zahtjev. Provjeravam sljedeće pjesme."}'
```

Reuse that ID only when recovering the same reply. `readThrough` is a receipt;
record accepted work and its eventual outcome separately with `work` below.
For a description, search catalogue
candidates and resolve its event ID with exact URIs:

```sh
python3 virtual-dj/dj.py describe 'ona Bebekova iz kafane gdje nije sklopio oka zbog ljubavi'
python3 virtual-dj/dj.py resolve 'DESCRIPTION_ID' 'spotify:track:2QQYG5M39oAaxz3hbzfCB8' --note 'Željko Bebek — Sinoć sam pola kafane popio'
```

Resolution returns choices to the person who asked; it does not enqueue them.
For each candidate, `resolve --matches /path/to/matches.json` can supply an array
of `{uri,basis,rationale,confidence,sources}`. `basis` is `title`, `literal_lyrics`
or `semantic`; optional confidence is a number from 0 to 1, and sources contain
`{url,label}`. The result retains this as `track.match`. Explain whether a clue
matches the title, actual lyric wording or the song's meaning. Retain the links
used for that judgment; supplied attribution is not server verification.

## Distinguish receipt, work and outcome

A read receipt means the message was seen. Acceptance records when work began.
An orange active marker means work is currently being performed; review,
deployment waits and blocked work have their own states.

```sh
python3 virtual-dj/dj.py work active 123 --summary 'Checking the requested transition'
python3 virtual-dj/dj.py work waiting 123 --summary 'Implementation is awaiting native review'
python3 virtual-dj/dj.py work completed 123 --outcome 'The requested change is deployed' --evidence 'Actual commit and observed deployment result'
python3 virtual-dj/dj.py message-info 123
```

Use real message IDs and evidence. `completed` requires an outcome and evidence;
`cancelled` requires a reason. Active leases expire to `unconfirmed`, preserving
the recorded work instead of inventing completion. Read receipts, replies and
the passage of time do not finish work. Reconcile old unresolved items against
the request inventory and actual delivered changes.

`message-info` is authenticated. It returns stored caller-declared surface,
guest ID, actual HTTP peer, operator label and delivery/acceptance/read times.
These identify an observed submission path. Legacy messages without origin
metadata remain unknown. Explicit fixture submissions have a test role and
event kind, so they do not enter the ordinary guest instruction stream.

## Change the show without interrupting the song

```sh
python3 virtual-dj/dj.py cue 'Robot je došao samo na jednu pjesmu. Prije tri sata.'
python3 virtual-dj/dj.py visual cocktail
python3 virtual-dj/dj.py visual --clear
```

A visual cue binds to the current occurrence UID. It yields to the next song
automatically. See [visual plugins](plugins/visuals.md) for the motif vocabulary,
procedural robots, ten-minute acts and extension points. Vary rhythm, scale,
colour and humour in response to the room; the archived show is a mild seed.
Book/photo material is optional and can remain disabled.

## Rehearse speech and karaoke quietly

```sh
python3 -B virtual-dj/rehearsal.py --json
python3 -B virtual-dj/render_voice.py --json
```

The first command exercises timing, identity, cancellation, mixer ownership and
failure paths under a guard that blocks processes and network access. The second
allows only the reviewed file-render commands, measures actual Croatian speech
and an offline synthetic mix, and removes its WAVs unless export is requested.
Neither needs the production player. The full procedure and result meanings are
in [REHEARSAL.md](REHEARSAL.md).

Prepare a dedication before its selected occurrence starts. Bind karaoke to the
exact recording and occurrence, and stop interpolation at the source clock's
deadline. Speech is disabled on new installs until deliberately configured.
Pause, skip, output changes and missed preparation cancel the old announcement.
Speech process completion and measured audio quality are separate from hearing
the intended words on the intended speakers.

At closing time, preserve the host's playback decision. Silent engineering,
screenshots and rehearsal need no pause, resume, provider switch or audio-routing
change. A rare external-source performance needs a separately prepared handoff;
an ordinary request or failed dedication stays in the Spotify workflow.

## Maintained material

| Material | Purpose |
| --- | --- |
| [PLAYBOOK.md](PLAYBOOK.md) | Audience signals, fast queue decisions, requests, transitions and performance procedures |
| [LESSONS.md](LESSONS.md) | Concrete failures from the night and the mechanisms that address them |
| [DECISIONS.md](DECISIONS.md) | Exact user asks, later corrections, disputed attribution and implementation outcomes |
| [seeds/music-directions.json](seeds/music-directions.json) | Broad musical directions and verified examples, open to expansion |
| [plugins/visuals.md](plugins/visuals.md) | Reusable visuals and source history |
| [REHEARSAL.md](REHEARSAL.md) | Silent checks, actual file synthesis and performance readiness |
| [Service reference](../tools/party_dj/README.md) | API/CLI details, state, installation and recovery |
| [Native stage](../docs/party-stage.md) | Build, private UI checks and native controls |

The command entry is `dj.py`; implementation stays in
`tools/party_dj/desk.py` beside the service. Use `--help` for individual commands.
Global `--url`, `--data` and `--operator` options precede the command. Service
credentials stay in the local state directory and are never guest-page data.
