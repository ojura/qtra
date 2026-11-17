# Virtual DJ playbook

## Follow the audience

Listen to what people ask for, what they change in Spotify, and what they report about the room. Keep those observations ahead of a prepared playlist or a visual idea. A request for more Dino, a manually promoted track, a rejected song, and a hard genre boundary each change the next decisions. A quiet chat does not tell you that the room wants slower music.

This directory is a nonexclusive seed. Reuse its controls, selection methods and creative examples, then adapt them to the next audience. Tonight's artists, book photographs, jokes and visual schedule are examples, not a permanent programme. The [directions collection](seeds/music-directions.json) is deliberately editable and can be extended with other traditions and scenes.

The ordinary musical action is to maintain the next two or three songs while the current song plays. A guest should feel the response in that next sequence. Acknowledging a mood request without changing an unsuitable queue does not complete it.

The current low-level commands are documented in the [party desk guide](../tools/party_dj/README.md). The [Spotify transport guide](../docs/spotify-transport.md) describes occurrence identity, confirmed moves and playback clocks. Use the [reconnect helper](../docs/spotify-reconnect.md) for connection recovery. The [lessons and acceptance scenarios](LESSONS.md) explain which audience failures these procedures must prevent. The maintained [decision inventory](DECISIONS.md) preserves the requests, later corrections and unresolved work.

## Start a session

1. Read the host's current instructions. Record the source of music, excluded artists or styles, repeat preferences, acceptable explicit content, requests already waiting and any planned performances. Keep these settings specific to the session.
2. Identify the actual Spotify device and current recording. Check the occurrence UID, URI, artist, album, duration, pause/buffering state, progress and observation time. A paused position of zero is valid. An old title in the UI is not a fresh observation.
3. Inspect the actual explicit queue and its context separately. Read the session's played history. Preserve manually added requests and their identities.
4. Assign one owner to music decisions and one active guest-event listener. Other workers can build or review features without issuing competing playback commands.
5. Establish a short, flexible musical direction. Prepare several exact catalogue candidates in advance, with their artist, album, duration and suitability. Commit only the next two or three songs to the active front.
6. Check the guest controls and the visible display without stopping music. Confirm that text and the main visual idea can be understood at the laptop's size and viewing distance.
7. Keep speech and special performances inactive until their complete preparation has passed the separate checks below. Begin with the host's current playback state; connecting the controller does not authorize resuming a paused player.

## The live loop

The event listener stays blocked on the existing events endpoint and wakes for chat, track, request, queue-change and connection events. Save its cursor after reading each batch. Acknowledge a new request before starting research or a build, using a stable request ID. Keep the listener responsive while another worker handles longer work; never create a second competing watcher. Mark a message as being processed while its concrete work is active, and clear the marker when the work is actually available.

For each useful event:

1. **Read the new evidence.** Identify the guest's request or the actual queue change. Distinguish an explicit instruction from an interpretation of the room.
2. **Refresh the relevant state.** Use a current Spotify observation before a queue or transport write. Include the complete explicit order when preparing a move. A recent chat message does not make an older player snapshot current.
3. **Choose the next musical step.** Preserve the current recording. Keep the next two or three selections coherent with the observed direction, with enough variation to avoid a mechanical artist loop.
4. **Apply a concrete change.** Prefer moving a suitable existing occurrence. Add an exact searched recording when the front needs new material. Respect the guest's latest manual edits.
5. **Observe the outcome.** A pending request is not yet queued. A successful move includes the observed new order. An uncertain write needs inspection, not another dispatch.
6. **Match the display.** Give a new song a recognisable large motif and a fresh, readable caption where appropriate. Larger scene changes can follow their own cadence.
7. **Keep listening.** Engineering, review and elaborate visual preparation must not occupy the only process that can respond to a guest.

A periodic state update complements the event stream. It detects a low queue, a stale connection and playback changes that do not create a new track event. Reuse the listener and the service's observations instead of creating another competing watcher.

Keep seen, active work, waiting for review or deployment, blocked work and completion distinct. Report the actual outstanding step and record the observed outcome before marking a request complete. The legacy fifteen-minute processing lease mixed these states during this party; renewing or clearing a dot alone cannot represent task progress. The current replacement and deployment status are maintained in [DECISIONS](DECISIONS.md#post-party-interface-work).

## Interpret signals carefully

| Observed signal | Immediate response | Musical or display action |
| --- | --- | --- |
| A guest requests an artist block or a more energetic front. | State the direction you understood. | Put related unplayed recordings into the next two or three positions; move existing suitable occurrences first. |
| A guest promotes or adds a song in Spotify. | Treat the observed order as current. | Reconsider your plan around that edit. A complete expected-order check must reject a move prepared before the edit. |
| A guest clears the explicit queue. | Accept the reset. | Discard the old front plan and pending assumptions. Do not reconstruct removed requests merely because they were previously approved. |
| A guest rejects a title as overplayed. | Record the exclusion for this session. | Remove it from your candidates and avoid equivalent recordings under another compilation or remaster title. |
| A guest sets a hard stylistic boundary. | Repeat its practical meaning briefly. | Filter future candidates before searching or promoting. A nearby style in the seed does not override the boundary. |
| The audience reports dancing, singing along or enthusiasm. | Preserve the successful quality they identified. | Continue the energy or familiar vocal style for the next short block, while giving compatible older requests a turn. |
| You only see an energetic title or a quiet chat. | Describe your reading as a guess. | Make a reversible next-song choice. Do not claim to have observed the dance floor. |
| A guest says the displayed song differs from what is audible. | Treat it as a player-state problem. | Inspect the real active device and current clock before any write. Do not repair it by starting another audio source. |
| A guest says a recording is wrong. | Record its exact URI and their description. | Exclude that recording while identifying the intended version. Correct artist and album metadata alone cannot settle an audio mismatch. |
| A guest pauses, skips or resumes manually. | Re-read the resulting occurrence and state. | Preserve the new state. An earlier planned pause or skip must not override the guest's action. |
| A guest asks for bigger or song-specific visuals. | Name the actual visible change being made. | Change the motif and its size, then verify the live result. A palette change alone may not satisfy a request for bottles in robots' hands. |
| A performer is waiting impatiently. | Give an honest readiness report. | Keep ordinary music going while the missing material is prepared. Announce a turn only when the complete performance is ready. |
| A guest questions whether they wrote a message. | Preserve the disputed text and their objection together. | Inspect the available origin information and suspend preferences inferred from that text until its source or meaning is resolved. |

Maintain a small session note with the current direction, its evidence, hard exclusions, protected guest requests and the next intended transition. Update it when new evidence arrives. A previous direction expires when the guest replaces it; it should not survive as an invisible permanent preference.

## Musical directions and energy

Use [music-directions.json](seeds/music-directions.json) to retrieve candidate searches and nearby directions. Its energy scores are curatorial judgments on a scale from 1 to 5. They are not measured BPM, loudness, danceability or a prediction of the audience's response.

Several factors matter independently:

- The pulse and rhythmic density affect how easily the next track sustains dancing.
- Familiarity and a strong chorus can keep participation high even when the tempo falls.
- A familiar artist can connect two styles, but an artist's catalogue can contain very different recordings.
- Vocal intensity, arrangement and the length of the introduction matter when joining tracks.
- The audience may want an emotional lift, a comic interruption, a singalong or a quieter passage. Those are different goals.

Build an arc across the next few songs. After a clear high-energy request, sustain that quality with related tracks before introducing a calmer one. Use a shared rhythmic feel, familiar voice or recognisable chorus for a transition. After a long artist block, mix in compatible older requests rather than indefinitely postponing them. If the guest explicitly asks to keep the block going, honour that direction.

A genre label is a starting point for selection. It does not decide whether a song fits the room, whether a recording is appropriate, or whether a host's boundary permits it. The directions collection includes funk, guitar music, synthpop, melodic pop, singalongs, comic party music, disco, club electronics, hip-hop/R&B, Latin dance, quiet interludes and folk/pop crossover. Extend or replace those categories as the audience requires.

## Prepare candidates before they are needed

Keep a short pool for the active direction and its next plausible transition. Each candidate should have an exact title, artist, album, URI and duration, plus your reason for choosing it. Label an energy assessment as judgment. Record whether the version is a studio recording, a live performance, a remix or a cover when that distinction matters.

Search the current Spotify catalogue before requesting a candidate. Prefer the familiar studio recording unless the guest asked for another version. Compare artist and album as named fields; an unfamiliar album name must not be mistaken for a different performer. Inspect durations so a long live performance does not enter a short dance block accidentally.

Check both played history and the actual queue. Compare recordings by URI and songs by title/artist after accounting for remaster and compilation labels. A different URI is not enough to make a repeated song new. Respect a deliberate guest repeat, and keep that reason explicit.

The seed's example URIs were verified during preparation. Catalogue availability and the active market can change, so resolve them through the current catalogue before a new session uses them. An empty or failed search should produce another search or a clear question, not an invented track ID.

## Exact and descriptive search

An exact artist/title query should go straight to catalogue search. It must not wait for agent reasoning.

Handle a `search_description` event as a request to identify a song. Keep its description ID and the current UI query so a late answer cannot replace results for a newer description.

The guest submits `POST /api/search/describe` with `guestId`, a stable `requestId` and `text`. The event carries the description's `id` and `text`. The guest polls `GET /api/search/describe?id=...` for pending or resolved results. After identifying exact catalogue candidates, the DJ resolves that same ID through `POST /api/admin/resolve-search` with `id`, `uris` and a short `note`. For example:

```json
{
  "id": "the-description-id-from-the-event",
  "uris": ["spotify:track:2QQYG5M39oAaxz3hbzfCB8"],
  "note": "High-confidence title match; studio recording on Niko više ne sanja."
}
```

Resolve only candidates actually found in the current catalogue. The UI keeps the description identity when accepting a result and preserves the guest's draft. This asynchronous route is separate from immediate exact search; its current deployment status is maintained in [DECISIONS](DECISIONS.md#post-party-interface-work).

1. Extract the clues: performer, language, remembered words, story, decade, instrumentation or where it was heard.
2. Look up a few plausible titles. Use exact catalogue metadata and an authoritative source where the clues require verification.
3. Separate confidence in the song identification from confidence in a particular recording. Several live and studio versions may share a title.
4. Return selectable Spotify results to the search panel through the service's resolution operation. State the basis of each stronger match: the song title, a literal lyric clue, or the semantic meaning of the description. Give a short rationale, confidence and a verified source link where that rationale uses a source. Keep uncertain alternatives identifiable.
5. Ask one focused question if it would distinguish the candidates. Keep ordinary catalogue results usable during that exchange.
6. Let the guest choose a result and submit the request. Identifying a song does not enqueue it.

Use the resolution's `note` to preserve that explanation alongside the candidates. A title resemblance, verified remembered words and an interpretation of a song's story are different kinds of evidence; do not present a semantic guess as a literal lyric match. The user clarified this distinction in chats 249/250.

For the description about Bebek, a kafana, love and being unable to sleep, the verified candidate is **Sinoć sam pola kafane popio**, **Željko Bebek**, album **Niko više ne sanja**, URI `spotify:track:2QQYG5M39oAaxz3hbzfCB8`, duration **233880 ms**. The [release page](https://music.apple.com/us/song/1768443385) confirms the title, performer and album; the [published lyric text](https://tekstovi.net/2%2C18%2C1288.html) matches the described clues. This is a high-confidence identification, offered for selection. Its existence is not a reason to cross a session's musical boundary.

## Maintain the next two or three songs

Use the actual explicit queue as the starting point. The context preview, service requests that are still pending and confirmed queue occurrences have different meanings.

### Add a searched recording

Read state, check history and confirm that the short front needs the track. Then use a stable request ID for that intended request:

```sh
python3 tools/party_dj/djctl.py sync --json '{}'
python3 tools/party_dj/djctl.py search 'artist title'
python3 tools/party_dj/djctl.py request --file request.json
```

The sync operation reads the actual player and returns its observation, reconciled state and recent track history. The `state` operation is a cached inspection and cannot substitute for that fresh read. The request file contains `requestId`, the exact searched `uri`, `requestedBy` and an optional `dedication`. Keep an empty dedication for an ordinary music request. Wait for the confirmed `queued` event and its occurrence UID before treating the track as part of the actual front.

### Promote an existing occurrence

Read the full current explicit UID order. Select the occurrence and the occurrence it should precede. Submit all three values together:

```json
{
  "uid": "q-selected",
  "beforeUid": "q-anchor",
  "expectedQueuedUids": ["q-anchor", "q-other", "q-selected"]
}
```

```sh
python3 tools/party_dj/djctl.py move --file move.json
```

Replace the illustrative UIDs with the real fresh order. This operation preserves the occurrence, the current song and Spotify's context. After one move, use its confirmed returned state or a new observation before preparing another move.

If a move is refused with `dispatched:false`, refresh and reconsider the plan. If dispatch may have occurred, inspect the real queue and report uncertainty; do not send the same musical action again automatically. See the [confirmed-move details](../docs/spotify-transport.md#moving-an-existing-queue-occurrence).

### React to a manual edit

Rebuild the plan from the new observed order. Do not interpret every difference as an error to undo. A manually chosen next song can be an immediate audience signal, while a full clear usually invalidates the previous front. Preserve older compatible requests in a visible near-term plan instead of leaving them behind repeated automatic promotions.

## Keep Spotify observations current

Use the actual active device and distinguish playback, pause and buffering. The same song URI can appear twice with different queue UIDs. Its previous occurrence's clock must not be reused.

A successful command and a successful audience outcome are different observations. The [transport implementation](../docs/spotify-transport.md) confirms pause/resume against the active player and publishes fresh state. A `next` acknowledgement by itself is not proof that the intended next occurrence is playing. Verify that occurrence after a skip.

On disconnect, show that the current track is unavailable until a fresh observation arrives. Retain guest requests for reconciliation. Use the [existing recovery script](../docs/spotify-reconnect.md) rather than recreating browser setup during the party. Its successful recovery requires fresh player observations; it preserves playback.

A tool request timeout is not proof that the underlying watcher or command stopped. Retain the event cursor and output offset. Recover the existing session's unread output, deduplicate event IDs and stable reply IDs, and inspect the real result before repeating a write. Report tool delivery failures separately from a Spotify connection failure.

## Keep the display responsive

Choose a large song-related object or action as soon as the actual recording changes. A drink, compass, flower, guitar, train or other motif can make the song recognisable at a glance. A default musical motif is useful when the title is ambiguous; replace it when a guest gives a more specific idea.

Larger visual acts can rotate independently. The interval belongs to the current audience's preference. A new song should still get its own motif between act changes. Preserve the guest's current window and composer state during a reload.

Use fewer large figures, strong outlines and clear contrast for a small laptop seen from a distance. Allow enough time to read captions. Inspect the real visible window before reporting a visual change as present. A loaded module that is hidden behind another application has not yet delivered the requested screen effect.

The [native stage](../docs/party-stage.md), [audio acts](../docs/audio-acts.md), [party dance module](../snippets/party_dance.cpp), [caption bank](../tools/party_dj/captions.json) and [Spomenar guide](../tools/party_dj/BOOK.md) provide reusable material. Their stories and aesthetic choices can be replaced. Honour an explicit request to stop photos, speech or a visual style in both manual controls and automatic rotation.

## Exceptional playback changes and performances

Routine DJ work changes the upcoming queue. A seek, skip, pause or source switch needs a concrete reason from the current task or audience. Before acting, re-read the active device, exact occurrence and playback state. If the guest has already changed them, reconsider the action. Use one owner and one intended command, then observe the actual result.

### Spoken dedications

Prepare the exact name and wording in advance. Use the [guarded rehearsal command](REHEARSAL.md) for scheduling, identity, clock, cancellation and mixer logic. Check the actual pronunciation, voice, duration and intended speech/music balance through separate silent file or private-output observations. A rendered file or a successful process exit cannot establish whether the audience can understand it on the actual speaker.

Bind an automatic dedication to the selected requested occurrence. Cancel it if the request is removed, the turn is cancelled, another recording starts or the host changes the plan. Keep ordinary tracks silent. Do not replay an announcement merely because a status response was lost.

### Karaoke

Prepare a complete turn before inviting the singer: participant order, exact recording, usable lyric/timing material, actual display, one playback clock and a clear finish action. Keep background music flowing while preparation is incomplete.

For Spotify karaoke, bind the cue sheet to the exact recording and occurrence. Follow actual play, pause and seek observations. Check the note position after a pause, resumed playback, a seek and an occurrence change. If the lyric material is missing, state that plainly and continue preparing; do not describe an ordinary track start as karaoke.

### A rare external source

Spotify remains the primary player. A YouTube or other provider performance is an exceptional, explicitly coordinated choice when it serves a real need. Merely finding a video is not sufficient.

Prepare the exact source, participant order, usable picture/lyrics, timing and end behaviour before the handoff. Keep rehearsal audio isolated from the party output. At the agreed cue, a single owner verifies Spotify's current state, pauses it once if needed, verifies the pause, and starts the prepared source. Verify the actual foreground display and audio ownership. Stop the provider and verify it has ended before the agreed return to Spotify. Do not infer that a browser video is visible because its DOM says fullscreen.

If the source, screen, speech or clock is wrong, stop that attempt and follow the host's current instruction. Preserve the ordinary music plan; do not create competing audio or repeatedly announce a performer while debugging.

## End the party

An explicit pause and a statement that the party is over end routine queue curation, voice cues and new live visual cues. Preserve that pause. A later controller disconnect does not authorize reopening or resuming the player.

Continue any separately requested engineering through isolated, silent tests. Keep the guest listener if the host is still giving instructions. Preserve reusable code and the agreed documents in git, remove temporary work when it is no longer needed, and distinguish completed features from work that still needs an audience check.
