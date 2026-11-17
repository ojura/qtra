# Lessons and acceptance scenarios

## Judge the result in the room

The audience judged the karaoke and dedication attempts unsuccessful. The user reported inaudible speech, brief competing music, lost synchronization, a stopped song after a manual resume, the wrong performer order and the wrong visible application. Those reports define the outcome. An internal speech result of `played`, advancing media time or a completed command did not establish a successful performance.

The next version needs usable tools and practiced procedures before a party begins. [PLAYBOOK.md](PLAYBOOK.md) describes the operating behaviour. This file connects the recorded audience feedback to concrete implementation and rehearsal requirements. The cases are useful starting material; future sessions should extend them when a different audience or setup reveals another failure.

## What must change

| Audience evidence | Failure that matters | Reusable behaviour | Acceptance scenario |
| --- | --- | --- | --- |
| Chat29 asked for a listener that wakes immediately; chats170/171 asked to stop reasoning from scratch. | Engineering occupied the same attention needed to answer guests and manage the next songs. | Keep one event listener active, prepare catalogue candidates and use established operations. Separate music decisions from feature development. | A guest message arriving during a build wakes the listener, receives one reply and remains associated with its active work. |
| Chats74 and127 described a displayed artist different from the audible player. | The controller and public display could present old or inactive-player state. | Observe the real active device, occurrence and clock; reject synthetic local state and clear stale public metadata on disconnect. | Feed an old browser title and a newer active-player clock, then a disconnect. The actual player wins; stale metadata disappears. |
| Chats113 to115 reported that the guest resumed music and an agent stopped it again. | A later command overrode a newer human action. | Re-read the exact current occurrence and state immediately before an exceptional transport change. Assign one owner. | Change the active occurrence or resume state between planning and dispatch. The stale action is refused or reconsidered rather than sent to a new target. |
| Chats40,71 and96 to98 requested syllable karaoke and asked where the words were while the singer waited. | An ordinary recording was started before the requested lyric experience was ready. | Prepare the exact recording, cue sheet, rendering and timing before the invitation. | A turn with missing lyrics or a mismatched recording cannot reach ready status or issue its invitation. |
| Chats110/111 reported inaudible speech and momentary parallel music. | Process success and intended ducking were mistaken for a clear announcement. | Rehearse the exact voice and phrase with measured speech/music levels on an isolated output. Bind the announcement to the intended turn. | Render and capture the phrase with music in a private sink; verify envelope, duration, clipping, cancellation and restoration without audible laptop output. |
| Chats119 to124 reported desynchronization, the wrong visible title and the wrong performer order. | Browser state, visible desktop state, audio and the invitation were not coordinated as one turn. | Prepare and verify participant order, source identity, visible window and a single clock before one controlled start. | A correct hidden browser DOM must not count as a visible performance. A stale source or changed participant order prevents the cue. |
| Chats130/131/135 and172 made Spotify primary and external sources exceptional. | Background provider playback disrupted the ordinary DJ task. | Use Spotify for the normal set. Require a concrete exceptional need and one coordinated source handoff. | Ordinary search, provider-link inspection and rehearsal never start a second audible source or pause Spotify. |
| Chats133 to139,149 and155 asked for active queue work that preserves the current energy. | Acknowledgements and append-only requests did not keep the front aligned with the room. | Maintain a rolling two- or three-song plan and promote compatible existing occurrences when appropriate. | A newly observed energetic direction causes a concrete suitable front, retains compatible guest requests and leaves the current clock advancing. |
| Chat145 rejected an overplayed song; the later queue was manually cleared. | Repetition or restoration could contradict a clear new preference. | Track exclusions and played history; accept a manual clear as a new starting point. | Different compilation/remaster URIs for a played title are filtered by default. A cleared queue is not reconstructed by an old plan. |
| Chats31,55,151 and153 requested fresher acts and later stopped book photographs. | Automatic rotation did not deliver the visual variety the guest was requesting. | Persist the current display preference and distinguish individual song changes from larger act changes. | A photo-off setting survives restart and rejects manual as well as scheduled photos. A new song changes its motif without resetting the act interval. |
| Chats159/160/165/166 asked for bottles/glasses, a visual per song and distance readability. | Small generic motion and a prepared-but-not-deployed change did not satisfy the visible request. | Use a recognisable large prop, check actual visible frames, and report deployment after that observation. | A laptop-sized private preview shows the held object clearly; a real new recording updates the motif while the sidebar and draft remain stable. |
| Chats93,107,112,175/176,179/182 and185/186 refined interaction. | Unclear processing, moving layout, intrusive scrolling, tiny pointer/low contrast and delayed exact search made the interface harder to use. | Keep feedback explicit, geometry stable and catalogue search immediate; implement the host's current interaction choices. | Replies do not pull an older reading position down before the current inactivity interval; own sends follow the true bottom; eligible undo restores the draft exactly. |

## Music curation needs an explicit plan

The request to keep the front energetic was not a request to interrupt the song. A useful response chooses the next few recordings, confirms their actual queue order and continues listening. The plan should record why those tracks fit the current signal and which older requests will follow.

The guarded move operation established during the party is useful because it retains Spotify occurrence UIDs and the full internal context. Its benefit depends on using it promptly for the audience's direction. A working move endpoint does not itself choose a coherent set.

Keep these cases in the queue rehearsal:

- A guest adds an energetic request while calmer songs are already queued.
- A guest requests a sustained artist block, then later asks to mix older requests back in.
- The same recording occurs twice with different UIDs.
- A manual reorder happens after a move is prepared.
- The guest clears the entire explicit queue.
- A move is acknowledged but the resulting order cannot be confirmed.
- An add request loses its response after dispatch.
- The current song changes naturally while a plan is being calculated.

The expected result is a deliberate next sequence, one dispatch for each intended action, preserved guest edits and an unchanged current performance except where the guest explicitly requested transport control. The [transport tests and implementation](../docs/spotify-transport.md#verification-and-deployment) cover the operation itself; the seed's direction examples add the musical decision scenarios.

Treat artist and album as separate named fields. A discussion during the set briefly misread the album **Svicky** as an artist mismatch for **Ritam moj**. Exact Spotify metadata and the label's release information confirmed **Dino Dvornik** as performer. A corrective move would have been unnecessary. The playbook therefore asks for explicit artist/album labels and verification before a title-based correction.

## Silent performance rehearsal

The post-party request authorizes thorough testing in silence. Keep the actual Spotify player paused and preserve the user's output routing. Use isolated player fixtures, a private Qt display and a private audio output or captured files for rehearsal. An application-level mute alone is not enough if another process can still reach the party speaker.

At the current consolidation checkpoint, the native pause is confirmed. The investigation permits guarded pure tests, owned private fixtures and reviewed offline file rendering; live player control, audio output, native input and deployment remain on hold. The unexpected resume and disputed-message source remain unresolved. The maintained [current state](DECISIONS.md#current-state) records changes to this boundary.

The implemented [guarded rehearsal command](REHEARSAL.md) runs the karaoke and speech logic suites with fake clips, processes and Pulse streams. Its successful result covers those scheduling and mixer checks. The remaining rendering, synthesis, captured-audio and pronunciation observations below have separate results; later intelligibility on the intended speaker needs human listening.

A complete rehearsal must answer these questions:

1. **Is the intended material ready?** Use an original fixture or usable supplied lyric/timing material. Check the exact recording URI, duration, participant and sequence. Missing material should remain a clear preparation state.
2. **Does one clock control the words?** Exercise start, pause, resume, forward/backward seek, buffering, stale observations and a repeated URI with a new occurrence UID. The note must hold or move according to the actual state.
3. **Is the intended content visible?** Inspect a privately rendered frame at native and laptop viewing sizes. Include a competing always-on-top window in the fixture; a hidden player cannot satisfy a visibility check.
4. **Can the phrase be understood?** Pre-generate the exact Croatian name and sentence. Check duration, pronunciation preparation, speech/music gain, clipping and the full duck/restore envelope in captured audio. Keep a clear distinction between these measurements and later human listening on the actual speaker.
5. **Does cancellation work?** Cancel before dispatch, during preparation, during speech and after the guest changes track or volume. Preserve a newer user adjustment instead of restoring an old absolute volume.
6. **Can a result be recovered?** Lose a tool response after an action, restart the service and replay retained observations. Stable request IDs and occurrence history must prevent a repeated invitation or source start.
7. **Is there one audio owner?** A simulated provider turn may start only after the intended Spotify pause is observed, and Spotify may resume only after the provider has stopped and the agreed return is still current.
8. **Does an ordinary request stay ordinary?** A search, a dedication draft and a provider URL do not start media. Empty dedications and cancelled turns remain silent.

The [karaoke format](../tools/party_dj/README.md#karaoke) and [native renderer](../snippets/party_stage.cpp) provide the existing clock and cue-sheet foundation. The [speech implementation](../tools/party_dj/speech.py) and its tests provide the starting point for isolated audio checks. Record concrete test results in the implementation's maintained documentation. Do not mark the performance ready because a file was generated or a status says it played.

## Connection and listener recovery

The party required repeated controller recovery and later encountered CoS MCP request timeouts. These are different failures.

For Spotify, use the [reconnect helper](../docs/spotify-reconnect.md), inspect the active device and require fresh observations. A sustained paused clock can be correct. At the end of this party the user explicitly said they had paused, so recovery work did not authorize playback.

For the agent's event delivery, keep the same watcher and cursor. A timeout can occur after output was read or after a write was dispatched. Recover retained output by original byte offset, assemble partial lines, and deduplicate handled event IDs. Preserve the raw response metadata needed to recover the next failure. A new watcher is justified only after the previous process has been shown gone and its last cursor has been recovered.

The guest's end-of-party instructions arrived during a tool timeout and were recovered from the existing stream. This case belongs in the rehearsal: new user directions must survive a delayed tool result, and the agent must stop party actions when the recovered message changes the task.

## Search and interface decisions

Exact catalogue queries should return without an agent reasoning step. Descriptive queries need a separate asynchronous identification flow that leaves the current composer and catalogue results usable. An old resolution must not overwrite a new query, and a candidate becomes a music request only after the guest chooses it.

The Bebek example is a useful semantic-search fixture because it combines a named performer, a setting and a remembered meaning. The identified title and exact studio recording are documented in the [playbook](PLAYBOOK.md#exact-and-descriptive-search). Keep alternative recordings selectable, with their album and duration visible.

The newest scrolling instruction is about two minutes without either a user or agent reply. Earlier twenty-second behaviour was superseded. Preserve manual reading, reset inactivity on the relevant interaction and new message, and keep own successful sends at the actual bottom after Qt finishes layout.

The requested Escape undo is limited to the latest message that has not been picked up for processing. Withdrawing it and marking it as being processed must be one consistent decision. A race must have a clear winner. A successful undo restores the exact text for editing; an in-progress message remains visibly in progress. The guest's duplicate message illustrates why this matters.

Chats 255/264-266 expose a second status problem. The legacy manually renewed processing flags mix active work, review/deployment waiting and unresolved requests. A status dot needs a durable task state and a real completion outcome behind it. The user explicitly rejected a cosmetic mass-change to completion checks. Exercise the lifecycle through worker delay, a waiting review, cancellation, failure and verified deployment, and show the corresponding state without pretending each open task is actively executing.

A larger cursor inside DJ mode and stronger scrollbar thumb/track contrast are separate changes. The initial global pointer enlargement was restored after chat200 limited the scope to the DJ interface. The guest explicitly accepted a thin scrollbar, so increasing its width would not address the stated contrast problem.

Unexpected text also needs a defined response. Chats199/212 disputed the origin or authorship of earlier text. Preserve that objection, check the available message-source data and suspend interpretations of the disputed wording. The old chat schema has no guest or input-source field, and both native and web clients use UUID request IDs, so attribution remains unresolved. Do not turn an ambiguous phrase into a lasting musical or interface rule.

## Source references

The local party archive is `~/.local/state/larics-dj/party.sqlite3`. Its `events` table stores guest messages as `kind='chat'`, with the numbered chat ID in the JSON payload. Read that database in read-only mode when checking a claim.

The references above use chat IDs, not event IDs. Key requests are:

| Chat IDs | Subject |
| --- | --- |
| 29,74,127,147 | Listener latency, repeatable connection setup, current-player coordination and manual queue monitoring. |
| 40,71,96-98,103,110-124 | Karaoke content, waiting performers, voice quality, inaudible speech, source overlap, timing and performer order. |
| 130,131,135,141,172 | Spotify primary, coordinated rare provider use, cancelled attempts and return to queue curation. |
| 133,134,139,145,149,155,161 | Musical direction, active queue front, repetition and the session's genre boundary. |
| 31,55,151,153,159,160,165,166 | Visual variety, photos off, song motifs, visible glasses and distance readability. |
| 93,107,112,128,175-179,182,185,186 | Processing feedback, scrolling, Tab navigation, search, undo, cursor and contrast. |
| 170,171,187-189 | End of party, reusable nonexclusive seed, full decisions inventory and two review rounds. |

The complete [request and decision inventory](DECISIONS.md) preserves exact user text, later corrections, cancellations and unresolved findings. These lessons connect those records to operating procedures and acceptance cases.
