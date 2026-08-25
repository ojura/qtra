# Runtime-agent protocol

Transport: newline-delimited UTF-8 JSON over a user-only `QLocalServer` Unix
socket. The implementation limits a pending request buffer to 1 MiB.

## Message forms

Request:

```json
{"id":1,"command":"cube.state","params":{}}
```

Success:

```json
{"id":1,"ok":true,"result":{"running":true}}
```

Failure:

```json
{"id":1,"ok":false,"error":{"code":"object_not_found","message":"..."}}
```

Event:

```json
{
  "event":"operation.finished",
  "sequence":"31",
  "monotonicNs":"918221044",
  "data":{"operationId":"4","outcome":"completed"}
}
```

Integer identities and counters that may exceed JavaScript's exact integer range
are encoded as decimal strings.

## Commands

### Process and discovery

- `hello`
- `help`
- `process.quit`
- `symbol.resolve {name}`

### Cube

- `cube.state`
- `cube.pause`
- `cube.resume`
- `cube.reset`
- `cube.speed {degreesPerSecond}`
- `cube.wireframe {enabled}`
- `cube.capture {path?}`

### QObject and widgets

Object selectors use either `{objectName:"..."}` or `{id:"..."}`.

- `object.tree {selector?, maxDepth?}`
- `object.list`
- `object.describe {selector, includeValues?}`
- `object.get {selector, property}`
- `object.set {selector, property, value}`
- `object.invoke {selector, method}` — zero-argument methods only
- `action.trigger {selector}`
- `widget.click {selector}`

### Events

- `event.subscribe {all, prefixes}`
- `event.history {afterSequence?, limit?, prefixes?}`

The server retains the most recent 1,024 published events. `event.history`
supports reconnect/replay after a known sequence number; live subscription is
still preferable for high-rate observation.

The CLI convenience form is:

```bash
python3 tools/agentctl.py history --after 200 --limit 128 --prefix operation.
```

### Native modules and snippets

- `module.list`
- `snippet.load {path}`
- `snippet.run {moduleId, executor, target?, request}`
- `snippet.release {moduleId, executor?, target?, request?}`

Executors:

- `gui`: queued on the main window;
- `object`: queued on the selected QObject;
- `render`: queued for `CubeWidget::paintGL()`, with its context current.

`snippet.run` immediately returns an operation ID. Completion is reported through
`operation.finished` with kind `snippet`.

`snippet.release` calls the module's `release` entry point instead of `run`, and
reports through `operation.finished` with kind `snippetRelease`. A module that
does not declare one is answering rather than failing, and says so with
`no_release_declared`; that is a different thing from a release that ran and
failed, and the two are meant to be told apart.

Executor and target are optional because the agent records where each module
ran, and a release belongs where its install ran. The resolution order is the
request, then the last successful run, then the last attempt, reported back as
`executorSource` of `request`, `recorded` or `attempted`.

The third exists because a run that installs something and then fails records no
success while leaving that state behind, and the attempt is the only witness to
where it can safely be torn down. Guessing instead is worse than not releasing
at all: Qt requires `removeEventFilter` to run on the watched object's thread, a
timer to be killed from its own, and forbids deleting a QObject across threads,
and nothing checks thread affinity the way the scene snippets check the GL
context — so a wrong guess is a race inside the process rather than an error.

Errors worth handling separately:

| code | meaning |
|---|---|
| `no_release_declared` | the module has no release entry point |
| `no_recorded_executor` | it has never been run at all, so it installed nothing |
| `recorded_target_gone` | the object it last ran on no longer exists |

`module.list` reports `declaresRelease`, `hadSuccessfulRun`, `hadAttemptedRun`,
and the recorded `lastExecutor`/`lastTarget` for each snippet module. A module
that has attempted a run without completing one also reports
`lastAttemptedExecutor`, which is the interesting case: it installed something
and then failed.

### Byte stash

- `stash.list`
- `stash.get {key}` — returns `base64`, `size`, `monotonicNs`, `moduleId`
- `stash.drop {key}`

The host keeps opaque byte strings under caller-chosen keys and never interprets
them. A module that overwrites host state puts the original here so that code
written afterwards can restore it: a copy kept in the module's own memory dies
with that module's private state and is reachable to nothing else, which is what
makes an undo written later impossible rather than merely unwritten.

The namespace is flat on purpose. Scoping keys to a module would shut out
exactly the later repair module the stash exists to serve. The convention is
`<module-name>/<what>`.

An entry means what the displacement currently in effect displaced, not the
oldest value ever seen, so an install overwrites it; `stash_put` takes an
explicit overwrite flag and reports whether the key was already there.

Entries outlive the module that wrote them and go away only on an explicit
`stash.drop`. Deciding that a restore actually worked takes an observation no
module can make about itself, so dropping is the driver's call, not the
module's — and a buggy restore that corrupts instead of restoring must not
delete the only good copy as its last act. Dropping an entry whose displacement
is still in effect is how you lose the original: releasing then reports that it
could not restore, rather than writing whatever is in the buffer.

Consumers replay an entry only after checking the displacement it describes is
still in effect. For these snippets that check is that the face is still all
zeros; if it is not, something else has changed it since and replaying would
revert that.

Each entry is stamped by the host with its size, a monotonic timestamp, and the
id of the module that wrote it, so provenance does not depend on the depositor
reporting it honestly.

### Function patches

- `patch.load {path}`
- `patch.activate {moduleId, mode}` where mode is `dispatch` or `entry`
- `patch.rollback`
- `patch.status`

### Explicitly unsafe operations

Available only with `QT_RUNTIME_AGENT_UNSAFE=1` or `--unsafe-agent`:

- `unsafe.status`
- `unsafe.memory.read {address, size}`
- `unsafe.memory.write {address, base64}`
- `unsafe.crash`

Memory transfers are capped at 65,536 bytes per request.

## Events emitted by the demo

- `agent.connected`
- `cube.stateChanged`
- `cube.frame` (throttled)
- `cube.capture.finished`
- `action.triggered`
- `operation.started`
- `operation.progress`
- `operation.finished`
- `snippet.loaded`
- `snippet.log`
- arbitrary snippet-defined events
- `patch.loaded`
- `patch.activated`
- `patch.changed`
- `patch.rolledBack`
- `render.glInitialized`
- `render.glMessage`
