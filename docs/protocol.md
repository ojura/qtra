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

Executors:

- `gui`: queued on the main window;
- `object`: queued on the selected QObject;
- `render`: queued for `CubeWidget::paintGL()`, with its context current.

`snippet.run` immediately returns an operation ID. Completion is reported through
`operation.finished` with kind `snippet`.

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
