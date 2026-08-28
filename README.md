# Qt Runtime Agent / Hotpatch Cube

A deliberately sharp-edged Linux/GCC/Qt 6 prototype for giving an external agent
semantic and native control over a running optimized C++ application.

The demo application is a `QMainWindow` containing a continuously rotating
OpenGL cube. Its menus expose ordinary Qt actions and a simulated asynchronous
point-cloud job. An in-process agent listens on a user-only Unix-domain socket
and provides:

- QObject/widget/action discovery through Qt's meta-object system;
- property reads and writes, zero-argument method invocation, semantic clicks,
  screenshots, and event subscriptions;
- a bounded event replay buffer for reconnecting clients;
- execution of separately compiled project-native C++ snippets on the GUI
  event loop, a selected QObject's event loop, or inside `paintGL()` while the
  OpenGL context is current;
- OpenGL vendor/renderer/version state and asynchronous debug-message events;
- a stable C host ABI for snippets plus an intentionally unsafe route to real
  C++ objects, private fields, process symbols, and process memory;
- two function replacement mechanisms: an atomic dispatch slot and a real
  Linux/x86-64 entry rewrite with rollback;
- a build-oracle tool that derives a snippet compiler command from the owning
  translation unit in `compile_commands.json`.

This is not a production debugger or a security boundary. It is an experiment
whose operating principle is: **correct native code can do almost anything;
incorrect native code may corrupt or crash the process.**

## Validation

`./scripts/validate.sh` builds the Qt-independent core in three Release
configurations and runs the hotpatch tests against each: ordinary `-O3`, `-O3`
with GCC LTO, and `-O3` with Intel CET/IBT (`-fcf-protection=full`). Every run
calls the optimized built-in function, installs an x86-64 entry redirect,
checks the replacement behavior, rolls the original bytes back, and checks the
original behavior again. The CET configuration also checks that `ENDBR64`
survives, that the jump lands in the NOP area after it, and that a replacement
entry point without `ENDBR64` is rejected.

For each configuration the build oracle then derives a fresh `-O3`
shared-object command from that build's `compile_commands.json`, compiles
`patches/wobble_patch.cpp` with it, and puts the result through the same
redirect and rollback.

The Python tests run under `ctest` and cover compilation-command
transformation, retained-event replay, quiet event streams, and socket protocol
behavior.

`./scripts/validate.sh --with-gui` adds the Qt application and the
Xvfb/current-display smoke session, which needs the Qt 6 packages below.

## Architecture

```text
                       external model / operator
                                  |
                       JSON-lines over AF_UNIX
                                  |
       +--------------------------v--------------------------+
       | RuntimeAgent, in the application GUI thread         |
       |                                                     |
       | QObject registry       event publisher              |
       | semantic actions       operation lifecycle          |
       | DSO/snippet loader     symbol + unsafe memory API    |
       | dispatch patching      x86-64 entry hotpatching      |
       +-----------+-----------------------+-----------------+
                   |                       |
          queued Qt invocation       paintGL callback queue
                   |                       |
             GUI/object loop          current GL context
                   |                       |
       +-----------v-----------------------v-----------------+
       | MainWindow / CubeWidget / QtConcurrent demo job      |
       +------------------------------------------------------+

       compile_commands.json + source
                   |
         tools/compile_snippet.py
                   |
             exact GCC context
                   |
                snippet.so
```

The normal control plane remains semantic. Raw symbols, private-field access,
memory writes, and machine-code patching are deliberate escalation paths.

## Prerequisites on Debian/Ubuntu-like systems

The included dependency script runs the following class of installation:

```bash
sudo ./scripts/install_deps_debian.sh
```

Its Debian package command is:

```bash
apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build python3 \
  qt6-base-dev qt6-base-dev-tools libqt6opengl6-dev \
  libgl1-mesa-dri mesa-utils xvfb xauth
```

Required application components are Qt 6 Core, Gui, Widgets, Network,
Concurrent, OpenGL, and OpenGLWidgets. The CMake project currently requires Qt
6.4 or later.

One snippet has a dependency of its own. `agent_snippet_audio_pulse` reads the
system audio mix through PulseAudio, which on Debian-like systems means
`libpulse-dev`. The list above leaves it out, and the build looks for it with
`pkg-config` and skips that one target when it is absent, so everything else
configures and builds either way. Install it to get the beat visualizer:

```bash
apt-get install -y --no-install-recommends libpulse-dev
```

## Build

Optimized application and all modules:

```bash
./scripts/build.sh release
```

Equivalent commands:

```bash
cmake --preset release
cmake --build --preset release --parallel
```

Qt-independent hotpatch and Python tests only:

```bash
./scripts/build.sh selftest-only
```

The stronger optimized/LTO variant is:

```bash
./scripts/build.sh selftest-lto
```

The Intel CET/IBT variant is:

```bash
./scripts/build.sh selftest-cet
```

`release-cet` is the corresponding Qt application preset.

Run every Qt-independent configuration plus the build-oracle redirect tests:

```bash
./scripts/validate.sh
```

On a machine with Qt 6, `./scripts/validate.sh --with-gui` also builds the app
and executes the Xvfb/current-display smoke session.

The build exports `compile_commands.json`, which is the build oracle's source of
compiler, include, macro, language, target, and ABI context.

## Run

Use an existing display:

```bash
./build/release/qt_runtime_cube --unsafe-agent
```

Or let the wrapper create an Xvfb display and force Mesa software rendering:

```bash
./scripts/run_xvfb.sh --unsafe-agent
```

By default the Unix socket is:

```text
/tmp/qt-runtime-cube-<uid>.sock
```

Override it with `--agent-socket PATH` or `QT_RUNTIME_AGENT_SOCKET`.
`--unsafe-agent` enables raw memory read/write and deliberate crash commands;
it is not required for snippets or function patches.

## Basic control

```bash
python3 tools/agentctl.py call hello
python3 tools/agentctl.py call cube.state
python3 tools/agentctl.py call object.tree --params '{"maxDepth":3}'
python3 tools/agentctl.py call object.describe \
  --params '{"objectName":"cubeView","includeValues":true}'
python3 tools/agentctl.py call object.set \
  --params '{"objectName":"cubeView","property":"angularVelocity","value":180}'
python3 tools/agentctl.py call action.trigger \
  --params '{"objectName":"actionWireframe"}'
python3 tools/agentctl.py events --prefix operation. --prefix patch. --seconds 60
python3 tools/agentctl.py history --after 100 --prefix operation.
```

`--seconds` bounds how long `events` streams for. `--timeout` is a different
setting: it is a client option, so it goes before the subcommand name, and it
sets how long a single read waits. A quiet
stream simply starts another read interval, so raising it does not extend the
session.

The protocol is one compact JSON object per line. Requests have `id`, `command`,
and `params`. Replies have matching `id`, `ok`, and either `result` or `error`.
Events have `event`, monotonically increasing `sequence`, `monotonicNs`, and
`data`.

See [`docs/protocol.md`](docs/protocol.md) for the command inventory.

## Runtime-native snippets

A snippet is an ordinary ELF shared object with one stable exported initializer:

```cpp
extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet*
runtime_agent_snippet_init();
```

Its descriptor points to a function receiving `RuntimeAgentHost`. The host ABI
provides logging, structured events, QObject lookup, dynamic-symbol lookup,
request JSON, completion/failure, and monotonic time.

The sample `snippets/inspect_cube.cpp` is intentionally compiled with GCC
`-fno-access-control`. It includes the actual application header and directly
reads/writes private `CubeWidget` fields. This bypasses setters, signals,
clamping, undo, and every other invariant. That is the point of the unsafe
native path.

Load and run a prebuilt snippet:

```bash
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_inspect.so \
  --executor gui \
  --request '{"nudgeAngle":15,"setSpeed":120}'
```

Run inside the next OpenGL paint callback:

```bash
python3 tools/agentctl.py snippet SOME_RENDER_SNIPPET.so \
  --executor render --request '{}'
```

The prebuilt `agent_snippet_render_probe.so` reports the current GL context,
thread, vendor, renderer, viewport, program, framebuffer, and sample count from
inside `paintGL()`.

Run on the event loop owning a selected QObject:

```bash
python3 tools/agentctl.py snippet SOME_SNIPPET.so \
  --executor object --target-name cubeView --request '{}'
```

### Ready-made scene modifications

Six prebuilt snippets change what the widget draws. Each installs a direct
connection to `CubeWidget::frameRendered`, which is emitted at the end of
`paintGL()` while the context is still current and the depth buffer still holds
the cube, so a hook there gets correct occlusion without touching `paintGL()`.
All six read private `CubeWidget` state for the frame's angle, scale, tint,
elapsed time, and projection, and all six are compiled with
`-fno-access-control`.

```bash
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_orbit_sphere.so --executor render --request '{}'
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_catmull_clark.so --executor render --request '{}'
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_mobius_ring.so --executor render --request '{}'
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_pillow_cube.so --executor render --request '{}'
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_jack_in_the_box.so --executor render --request '{}'
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_audio_pulse.so --executor render --request '{}'
```

| module | what it does | parameters |
| --- | --- | --- |
| `agent_snippet_orbit_sphere` | shaded sphere on a tilted orbit | none |
| `agent_snippet_catmull_clark` | replaces the cube with its Catmull-Clark subdivision surface, keeping the six face colors | `level` 0 to 5, default 2 |
| `agent_snippet_pillow_cube` | turns each side into a stuffed pillow panel with sewn edges and stitching | `puff`, `roundness`, `tuck`, `resolution`, `seam`, `stitches`, `fabric` |
| `agent_snippet_mobius_ring` | rotating Mobius strip, translucent surface and opaque rim | `radius`, `width`, `tilt`, `spin`, `alpha`, `edgeInset` |
| `agent_snippet_jack_in_the_box` | hollows one face and puts a sprung jack inside it | `side`, one of `front`, `back`, `left`, `right`, `top`, `bottom`, default `front` |
| `agent_snippet_audio_pulse` | drives the cube and a rig around it from the system audio mix | `device`, `sensitivity`, `dynamicRangeDb`, `events` |

### Switching them on and off

All six add a checkable entry to the application's Cube menu when the snippet
is loaded, under a separator that marks off everything added at runtime:

| entry | shortcut | object name |
| --- | --- | --- |
| Orbiting sphere | `Ctrl+Shift+O` | `actionOrbitSphere` |
| Catmull-Clark | `Ctrl+Shift+C` | `actionCatmullClark` |
| Mobius ring | `Ctrl+Shift+M` | `actionMobiusRing` |
| Pillow mode | `Ctrl+Shift+P` | `actionPillowMode` |
| Jack in the box | `Ctrl+Shift+J` | `actionJackInTheBox` |
| Beat visualizer | `Ctrl+Shift+B` | `actionBeatVisualizer` |

The application has no knowledge of these snippets and must not acquire any:
they are separate shared objects that may never be loaded at all. Each entry is
therefore created by the snippet once it is in the process, and its handler is
code inside that module, which is one more reason a loaded snippet is
never unloaded.

Because the entry is an ordinary `QAction` in the application's object tree, the
semantic API drives it like any other:

```bash
python3 tools/agentctl.py call action.trigger --params '{"objectName":"actionPillowMode"}'
python3 tools/agentctl.py call object.get \
  --params '{"objectName":"actionMobiusRing","property":"checked"}'
```

The same flip is available over the socket as `{"toggle": true}`, and the
checkbox follows the effect whichever way it was switched:

```bash
python3 tools/agentctl.py call snippet.run \
  --params '{"moduleId":1,"executor":"render","request":{"toggle":true}}'
```

A menu click arrives on the GUI thread with no current OpenGL context, and both
installing and removing need one, so the handler defers the work to the next
`paintGL()` through `CubeWidget::enqueueRenderCallback()`, where a context is
current. Parameters survive being switched off: a snippet keeps
the values it was last given and reuses them when it comes back on.

A second copy of the same snippet loaded from a different path does not add a
second entry. It would carry the same name while driving different state, so the
module that got there first keeps the menu, and the later one is reachable only
through its own `moduleId`.

**They exclude each other through the claims they leave in the stash.** Three of
them replace part of the mesh: `catmull_clark`, `pillow_cube` and
`jack_in_the_box` displace some region of the widget's vertex buffer by
overwriting it with zeros, and record that in the host's byte stash: the
displaced originals under `cube.vertexBuffer/<offset>+<length>`, and a claim
alongside it saying a displacement is in effect. Installing asks whether any
claim overlaps the region it wants and refuses if one does, naming the region
and the snippet that holds it.

A claim is a record, so overlap between two of them is arithmetic: a one-face
displacement and a whole-mesh one compare directly, and neither has to guess the
other's granularity. It also means a snippet needs no list of its siblings, so a
snippet written later is accounted for without touching any of the ones already
here.

A value check also runs, answering a different question. The claim says who
owns a region; the values say whether it is displaced right now. Both are needed,
because a release that could not recover the originals leaves a region collapsed
with no claim on it, and recording those zeros as the originals is how a face is
lost for good:

```text
floats 0..36 of the widget's mesh are already displaced by "hollow one side of
the cube and hide a jack in the box in it"; saving what it wrote as the
originals would lose them
```

A byte record outlives its claim, so a region can be free while its record still
names the snippet that last displaced it. Whether that record stops a later
install depends on which of two rules applies, and the two work at different
granularities on purpose.

Claims are compared arithmetically. `cube_mesh::overlappingClaim` reads every
`/claimed` entry out of `stash_list` and intersects the float ranges, so a
six-vertex displacement and a thirty-six-vertex one see each other whatever
offsets they use.

Records are compared by exact key. `stash_put` refuses an overwrite when an
entry under *that key* belongs to another snippet, and returns -2 so the caller
can report it. Two snippets wanting the identical key therefore exclude each
other: running `pillow_cube` against a `catmull_clark` record left at
`cube.vertexBuffer/0+216` is refused with the key named. Two snippets at
different granularities do not: `jack_in_the_box` deposits at
`cube.vertexBuffer/0+36`, which is a different key, so it installs even though
that range sits wholly inside the record above.

This is the host's boundary, and it is deliberate. `stash_put` cannot know that
`0+216` and `0+36` denote overlapping float ranges without parsing a key schema,
and a host that parsed keys would not be the byte store described below, which
never interprets what it stores. Exact-key ownership is the strongest rule
available to something that does not read its own keys.
Range arithmetic needs to know what a key means, so it belongs to the domain,
which is why the overlap test lives in `snippets/cube_mesh.h` next to the layout
it understands. `stash_list` reports every key with its owner, which is all a
domain needs to build such a test.

The consequence is worth stating, because it cuts against what an entry is
declared to mean further down: an entry is *what the displacement currently in
effect displaced*, and a `catmull_clark` record at `0+216` stops meaning that
once `catmull_clark` has released and the jack owns `0+36` inside it. A repair
module replaying `0+216` would put factory vertices back over the face the jack
is holding open. Nothing is corrupted, because the jack replays only onto its
own zeros and declines when the face holds anything else, but the record is
stale in a way the host cannot detect and no claim marks. Clearing a record
nothing claims is a `stash.drop` the driver makes.

Also worth knowing from the menu: a refused install shows only as the checkbox
snapping back. The reason is in the module's log line and in the result of the
same install driven over the socket.

The overlays are a different matter: the sphere, the ring and the beat
visualizer only add draw passes, so they take no claim, compose with each other,
and compose with any of the mesh replacements.

Each also accepts `{"restore": true}` to take itself back off. The Catmull-Clark and
pillow ones have to suppress the widget's own draw, because `paintGL()` calls
`glDrawArrays(GL_TRIANGLES, 0, 36)` with the count baked into compiled code and
a denser mesh cannot go through that buffer. They keep the widget's 36 original
vertices, overwrite them with zeros so that draw rasterizes only degenerate
triangles, and put them back on restore.

The pillow's six panels are separate grids that share their border curves, so
the surface stays closed while each panel keeps its own normals along the seam
and shades as a crease. Its parameters trade off against each other: pushing
`puff` much past the corner radius set by `roundness` and `tuck` puts the panel
centers as far from the middle as the corners are, and the silhouette becomes a
ball. `fabric` fades the six saturated face colors toward a
linen tone; it defaults to 0, which keeps them exactly as the widget draws them.
Re-running the loaded module keeps any parameter the new request omits.

Re-run a loaded module, so that the module's static
state is the one that gets updated:

```bash
python3 tools/agentctl.py call snippet.run \
  --params '{"moduleId":2,"executor":"render","request":{"level":4}}'
python3 tools/agentctl.py call snippet.run \
  --params '{"moduleId":3,"executor":"render","request":{"restore":true}}'
```

Loading a *different* path gives a separate module with its own copy of that
state, which for these means a second draw hook, leaving the first in place.
`tools/jit_snippet.py` writes a fresh path every time, precisely so `dlopen()`
cannot hand back an already loaded object, so recompiling one of these and
running it again does stack a second copy.

Loading the *same* path twice is safe. The agent records a new module id per
load, but `dlopen()` returns the same handle, so both ids address one copy of
the module's static state and the second run reports that the modification is
already installed instead of installing another one. Either id works as
`moduleId`.

### Changing the application's own dialogs

`agent_snippet_about_signature` adds a line to the About box of a process that
is already running, with no edit to `src/main_window.cpp` and no restart:

```bash
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_about_signature.so \
  --executor gui --request '{"line":"claude was here"}'
```

The snippet installs an application-wide event filter. `QMessageBox` sees
`QEvent::Show` before it reaches the screen, so the application builds its
dialog exactly as it always does and the line is appended to whatever text that
box turned out to carry.

Appending is what keeps this correct. The wording lives in a lambda in
`main_window.cpp`, and a snippet cannot read a string literal out of compiled
code, so the only text a snippet can depend on is the text the application
itself produced. The filter appends to that and needs to know none of it.

Reading it back while the dialog is open gives:

```text
An optimized Qt 6/OpenGL process with semantic RPC, runtime-compiled C++
snippets, and two hotpatch modes.

claude was here
```

`{"restore": true}` removes the filter and reports how many dialogs it changed.

### Writing through the seam that owns the value

`agent_snippet_audio_pulse` drives the cube from whatever the machine is
playing. What generalizes from it is a rule about which seam to write through,
and that rule applies to any process this tooling is pointed at.

A value the target recomputes every frame has an owner: the code that writes it
each tick. Only writes made through that owner survive. Write the same field
from anywhere else and the write itself succeeds, the field holds the new value
for part of a frame, and the next tick overwrites it before anything draws with
it. Nothing reports an error, so the wrong seam reads like a bug in your own
code.

The cube's tint is exactly that. `advanceAnimation()` writes `m_angleDegrees`,
`m_tint` and `m_scale` from the step function's output once per tick, and it
does so before the next `paintGL()` reads them. Tinting the cube from the
`frameRendered` hook, where the other scene snippets do their work, therefore
has no effect at all.

So this snippet uses both seams at once, according to which owns what:

| seam | what it drives | why that one |
| --- | --- | --- |
| dispatch step function | the cube's angle, tint and scale | the widget recomputes all three every tick from the step's output |
| `frameRendered` hook | the geometry drawn around the cube | it runs while the context is current and the depth buffer still holds the cube, so the drawing is occluded by the cube for free |

Releasing restores whichever step function was installed beforehand, so a
dispatch patch loaded first survives this
snippet being switched on and off.

```bash
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_audio_pulse.so --executor render --request '{}'
python3 tools/agentctl.py --timeout 12 events --prefix audio.
```

Audio arrives from PulseAudio's `@DEFAULT_MONITOR@`, which is the monitor of
whatever the default sink currently is, so it follows the output the machine is
already using without being told which device that is. A capture thread runs a
2048-point FFT every 512 samples and reduces it to 64 log-spaced bands, spectral
flux onset detection with a separate detector over the low bands for kicks, and
a tempo estimate from the median interval between onsets. The GUI thread only
ever copies the result struct.

Each detected onset is published as an `audio.beat` event carrying its strength,
spectral centroid and the current tempo estimate, so a client can consume the
analysis without drawing anything. `{"events": false}` turns that off.
`sensitivity` and `dynamicRangeDb` tune detection and display range, and
`device` names a monitor source explicitly.

Its CMake target sits behind a `pkg-config` check for `libpulse-simple`. A
machine without that library loses this one target and configures normally.

### Project-aware on-demand compilation

Compile only:

```bash
python3 tools/compile_snippet.py \
  --compile-db build/release/compile_commands.json \
  --context src/cube_widget.cpp \
  --source /tmp/my_probe.cpp \
  --output build/release/runtime-snippets/my_probe.so
```

Compile, load, execute on the GUI loop, and wait for its completion event:

```bash
python3 tools/jit_snippet.py /tmp/my_probe.cpp \
  --compile-db build/release/compile_commands.json \
  --context src/cube_widget.cpp \
  --executor gui \
  --request '{"anything":"the snippet expects"}'
```

Compile and immediately hot-activate a replacement generation:

```bash
python3 tools/jit_patch.py patches/wobble_patch.cpp \
  --compile-db build/release/compile_commands.json \
  --context src/cube_step.cpp \
  --mode entry --optimization O3
```

Each automatically named output uses a fresh path because `dlopen()` otherwise
reuses an already loaded object with the same canonical filename.

The compiler transformer preserves the original compiler and working directory,
include ordering, defines, language dialect, architecture/ABI options, forced
includes, and similar context. It removes the original input/output, dependency,
optimization, LTO, profile, and PIE settings; then adds a chosen optimization,
`-fPIC`, `-shared`, and by default `-fno-access-control`.

Successful snippet modules are deliberately never unloaded. A snippet may have
installed a Qt callback whose machine code or static state lives in the module.
A real system would add generations and epoch/reference tracking before unload.

### Editing a loaded snippet without restarting

`dlopen()` keys loaded objects by pathname, so asking for a path that is
already loaded returns the resident handle and its code, whatever the file on
disk now says. Rebuilding a snippet target and loading it again therefore runs
the resident module, and reports success while doing it: a new module id comes
back, `snippet.run` completes, and nothing says the edit was ignored.

Getting the new code in takes two steps: tell the installed generation to
release whatever it installed, and present the object under a pathname nothing
has asked for yet. `agentctl.py reload` does both:

```bash
cmake --build --preset release --target agent_snippet_jack_in_the_box

python3 tools/agentctl.py reload build/release/agent_snippet_jack_in_the_box.so \
  --request '{}'
```

That copies the object to `runtime-snippets/<name>-<hash>.so`, loads it, finds
the generation it supersedes, asks that one to release what it installed, and
then runs the new one. The edited code is live in a process that was never
restarted.

The handover needs no payload and no executor, because the outgoing module
declares a release entry point and the agent knows where it ran. `--executor`
here names where the *new* generation runs; the handover follows the agent's
own record instead, so the two never have to agree.

`handover.outcome` is one of:

| outcome | meaning |
|---|---|
| `released` | the module's release ran and completed |
| `declared-none` | it declares no release; nothing was undone |
| `never-ran` | it has never been run, so it installed nothing to undo |
| `payload-sent` | `--handover-request` was sent instead; effect unverifiable |
| `failed` | a release ran and did not complete |

Only `failed` stops the new generation being run, and `--force` overrides that.
Nothing can be unloaded, so the new module stays resident and inert instead.
Without a handover the new generation would install alongside the old one, which
for two `frameRendered` hooks means two copies both drawing.

`--handover-request` covers a module that declares no release. It sends a
request payload and cannot report whether anything acted on it, which is the
difference the declaration removes.

Generations are matched by the descriptor's `name_utf8`, the same string in
every build of one source. Reloads and `jit_snippet.py` output do not share a
filename stem, so the name is the only stable key. `--replace ID` picks the
outgoing module explicitly.

`jit_snippet.py` is the tool for a source file with no CMake target. For one
that has a target, build the target: an incremental build costs about what the
oracle's own compile costs, and the built object is the artifact the project
produces, compiled with the project's own command line instead of the oracle's
reconstruction of it.

Two properties hold across a reload:

- Nothing is unloaded, so every iteration leaves its predecessor resident.
- A menu entry created by `scene_toggle` is handed to the newest generation.
  The entry is rebound, so the checkbox drives the code
  that is installed. This assumes the handover ran first.

Plain `agentctl.py snippet` on a path that is already loaded says so on stderr,
naming the path and pointing at `reload`.

Work queued for the `render` executor runs inside `paintGL()`, and `update()`
only asks for a repaint. A window the compositor has stopped sending frame
callbacks to, whether covered, unfocused or minimised, may never provide one,
and
queued work would then wait for as long as the window stays hidden while the
caller sees only its own request timing out. `CubeWidget` therefore starts a
100 ms single-shot watchdog whenever it queues render work, and a queue still
unserved when it fires is drained through `grabFramebuffer()`, which renders
whatever the compositor is doing. A visible window never reaches it. This lives
in the widget, so every caller gets it.

### Refusing a module built against a different host

A module compiled with `-fno-access-control` reads application members
directly, and `cube->m_angleDegrees` becomes a byte offset when that module is
compiled. The offset describes the source the module saw. Load it into a process
built from source where that member moved, and it reads and writes whatever now
lives at that offset. `RUNTIME_AGENT_ABI` says nothing about this, because
the agent's own interface is what it covers and the application's types are what
moved.

So each module is stamped with the GNU build id of the executable it was
compiled against, and the agent compares that with the build id of the running
executable before the module runs:

| the module reports | what happens |
|---|---|
| the running build id | it loads |
| a different build id | it is refused and closed, and the error names both ids |
| nothing | it loads, and the result reports that its offsets are unchecked |

An unstamped module loads because the host cannot tell one built outside this
build system from one built before the last change to the application. Refusing
both would decide which toolchains may produce a module, and the question here
is whether the offsets are right. `module.list` reports `stamped` for every
module, `snippet.load` says so in its result, and the agent logs a warning.

The identity is the note the linker already writes, so the application carries
no code for this. CMake reads it after the executable links, writes it into a
generated header, and force-includes that header into every module target, which
is why no snippet source mentions it. `tools/compile_snippet.py` stamps the same
value by reading the executable beside the compile database, so `jit_snippet.py`
and `jit_patch.py` produce stamped modules too. Pass `--no-build-id` for one
that is deliberately unstamped.

The check is as coarse as the build id, which changes whenever any translation
unit does. A module is therefore refused after a change that moved nothing it
reads. Rebuilding it is the answer, and an edit that is reverted produces the
original id again, so the modules built before it are accepted once more.

`hello` reports the running `buildId`, which is what a module has to match:

```bash
python3 tools/agentctl.py call hello
```

### Declaring how a module lets go

A snippet that installs something lasting can declare how to undo it, as a
fifth field in its descriptor:

```cpp
const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI, sizeof(RuntimeAgentSnippet), "...", &run, &release,
};
```

`release` receives a host and reports through `complete_json`/`fail` exactly as
`run` does, so a release that fails is an error a program can act on.

`RUNTIME_AGENT_ABI` is bumped whenever either ABI struct changes shape, and
the loader accepts only an exact match: a module that disagrees with the host
about either struct fails to load, so it cannot run against fields the host
never wrote. There is no adaptation path and none is intended. Rebuild the
snippets when the header changes; a stale `.so` under `runtime-snippets/` is
rejected at load.

A null `release` declares that the module has nothing to undo. Of the ten
snippets here, `inspect_cube`, `install_observer` and `render_probe` install
nothing that outlives the call, and `install_observer` says as much in its own
result.

```bash
python3 tools/agentctl.py call snippet.release --params '{"moduleId":1}'
python3 tools/agentctl.py call module.list
```

No executor is needed. The agent records how each module was last run
*successfully*, and runs the release there. The last success, since a snippet
needing a GL context fails under `gui` with "use executor=render" and is
retried. `module.list` reports that record alongside `declaresRelease`. Passing
`executor` or `target` overrides it; a recorded target that no longer exists is
reported as `recorded_target_gone`.

Release must not report completion before its effects are applied, because a
handover is sequenced on that completion. The scene snippets refuse for this
reason: releasing one under `gui` returns an error telling you to use `render`,
because a deferred teardown would report success while the cube is still
collapsed.

What this does not do is verify the effects are gone. It establishes that the
release ran and reported, and nothing beyond that: a buggy release that leaves
half its state behind reports success. The declaration is also one bit per
module, so a snippet that installed three things cannot say it can undo two. The
install-time defences, such as the scene snippets refusing to save an
already-collapsed vertex buffer, matter for that reason, because nothing
distinguishes "nothing to release" from "the author forgot".

### The byte stash

A module that overwrites host state can put the original where the host owns it:

```cpp
host->stash_put(host->agent_context, "jack_in_the_box/face-front",
                vertices.data(), byteCount, /*overwrite=*/1);
```

The reason is narrower than "plan your cleanup". Undo *code* can be written
later: Qt keeps connection, filter and action state, so a module written
afterwards can disconnect a hook it never installed. Undo *data* cannot be
reconstructed once it is gone. The jack's six original vertices would otherwise
live only in its own `static` behind an anonymous-namespace pointer, so a later
repair module could remove the draw hook and never put the face back.

An entry means *what the displacement currently in effect displaced*, not the
oldest value ever seen, so installing overwrites it. Keeping the first copy
forever would replay factory vertices over a later legitimate edit and revert it
silently.

Two rules follow from that meaning, and both are matters of placement:

- **Deposit behind the guard that makes the install safe.** The jack refuses to
  install onto an already-collapsed face, and the deposit sits after that check,
  so nothing can reach it with bytes the module has not just validated. There is
  no separate obligation on whoever writes the next snippet.
- **Replay only onto your own displacement.** Before writing the saved bytes
  back, the jack checks the face still holds the zeros it wrote. If something
  else changed it since, the bytes are left alone and the result says so. That
  check is what makes an entry which outlives its release safe to keep.

The stash holds the *only* copy. The jack keeps no private duplicate and its
own restore reads it back from there. That is deliberate: a saved copy nobody
reads is a copy nobody notices going wrong, whereas here the bytes another
module would rely on are the bytes this module's own restore needs. Drop the
entry while the face is still open and releasing reports that the face could not
be restored, leaving the buffer untouched.

```bash
python3 tools/agentctl.py call stash.list
python3 tools/agentctl.py call stash.get --params '{"key":"jack_in_the_box/face-front"}'
python3 tools/agentctl.py call stash.drop --params '{"key":"jack_in_the_box/face-front"}'
```

The host never interprets the bytes. It stamps each entry with size, a monotonic
time, and the id of the module that wrote it, so provenance does not depend on
the depositor reporting it honestly. The namespace is flat on purpose: scoping
keys per module would shut out exactly the later repair module the stash exists
to serve. The convention is `<module-name>/<what>`.

Entries are not dropped when a module releases. Deciding that a restore actually
worked takes an observation no module can make about itself, so that call
belongs to whoever is driving; and a restore that corrupts must not delete the
only good copy as its final act.

This is not an undo log. The host stores bytes and has no idea what they mean,
which is the point: a host that had to replay effects would need a vocabulary
covering everything a snippet might do, and that contradicts snippets being able
to do almost anything.

Not everything needs saving. `cubeVertices` is `constexpr` in an anonymous
namespace, so it is absent from `.dynsym` and `symbol.resolve` misses it, but it
is still in `.symtab` and readable from the live process with
`unsafe.memory.read` at a load base derived from any resolvable symbol, and it
is in the repository as source besides. The rule is to save what you overwrite
*unless it is reconstructible from the binary or the source*.

## Function hot-swapping

### Dispatch mode

`CubeWidget` performs one atomic function-pointer load and indirect call per
animation tick. This is the predictable, portable seam:

```bash
python3 tools/agentctl.py patch \
  build/release/cube_patch_wobble.so --mode dispatch
python3 tools/agentctl.py call patch.rollback
```

Unpatched overhead is one atomic load plus an indirect call per tick.

### Raw entry mode

`cube_step_builtin` is built as:

```cpp
__attribute__((noinline, noclone, patchable_function_entry(16, 0)))
```

On Linux/x86-64 the patcher verifies that all 16 reserved bytes are NOPs, saves
them, temporarily changes text-page permissions, and writes:

```text
movabs r11, <replacement address>
jmp    r11
```

Future entries jump to the replacement DSO. Rollback restores the exact saved
bytes.

With `-fcf-protection=full`, GCC places `ENDBR64` before that NOP region. The
patcher preserves the four-byte landing pad and writes the jump at `entry + 4`.
Because the jump itself is indirect, it also requires the replacement function
to begin with `ENDBR64`. The `release-cet` and `selftest-cet` presets apply the
same CET setting to the host and runtime modules.

```bash
python3 tools/agentctl.py patch \
  build/release/cube_patch_wobble.so --mode entry
python3 tools/agentctl.py call patch.status
python3 tools/agentctl.py call patch.rollback
```

The socket server and animation run on the GUI thread. Patch activation pauses
the timer and happens at a GUI-thread request boundary, so this sample has no
concurrent caller of the target while its 13-byte jump is written. That is a
property of this demo, not a general stop-the-world implementation.

Entry rewriting does not affect inlined copies, compiler clones, folded calls,
or callers optimized around the function. A general agent must inspect the final
ELF and call sites, patch relevant clones/callers, or rebuild those callers.

## Unsafe memory and symbols

The executable is linked with `--export-dynamic`, so dynamically visible host
symbols can be resolved by name:

```bash
python3 tools/agentctl.py call symbol.resolve \
  --params '{"name":"cube_step_builtin"}'
```

With `--unsafe-agent`:

```bash
python3 tools/agentctl.py call unsafe.memory.read \
  --params '{"address":"0x...","size":32}'
python3 tools/agentctl.py call unsafe.memory.write \
  --params '{"address":"0x...","base64":"..."}'
```

These use `process_vm_readv`/`process_vm_writev` against the current PID. They
can observe or corrupt any mapped user-space state permitted by the kernel.
`unsafe.crash` intentionally dereferences null to verify crash collection.

`dlsym` does not expose every ordinary ELF symbol. A fuller implementation would
index `.symtab`, DWARF, build IDs, loaded-module biases, and Ninja object
provenance, then pass typed raw addresses to runtime-compiled objects or an
in-memory ELF linker.

## End-to-end test

After a Qt build:

```bash
python3 tools/smoke_test.py --build-dir build/release --keep-runtime
```

The test launches the application on the current display or Xvfb, connects to
the socket, queries the object tree, exercises dispatch and entry patching,
loads the private-state, persistent-observer, and render-context snippets,
replays retained completion events, captures a PNG framebuffer, and requests a
clean process exit. Logs, screenshot, and transcript are kept when requested or
when a failure occurs.

## Important limits

- The entry patcher is Linux/x86-64 only and supports prepared all-NOP entries;
  it does not relocate arbitrary prologues.
- Writing 13 bytes is not atomic. General multi-threaded targets need cooperative
  quiescence, thread suspension, trap-assisted staging, or a different patch
  strategy.
- Only successful DSO loads are retained; a failed load is closed.
- The semantic registry covers the main-window QObject tree and explicitly
  registered objects; non-QObject domain state needs generated/manual adapters.
- `object.invoke` currently handles only zero-argument methods.
- QObject property operations are executed by the GUI-thread socket handler.
  Worker-thread-affine objects should be accessed through a queued native
  snippet targeting that object.
- An optimized-away source variable may have no recoverable runtime value. The
  agent can use DWARF and assembly where possible, or hot-install a rebuilt probe
  that explicitly materializes the value on the next execution.
- A matching build id says a module was compiled against the running build. It
  says nothing about whether the module is correct: the offsets it holds
  describe the types this process has, and what it does through them is its own
  business.

## Repository map

```text
include/agent/agent_abi.h       stable snippet host ABI
include/demo/cube_step_abi.h    patch function/descriptor ABI
src/agent/runtime_agent.*       JSON socket and command dispatch
src/agent/object_registry.*     QObject IDs and reflection
src/agent/module_manager.*      dlopen, snippet, dispatch, entry patch state
src/agent/build_id.*            the running executable's GNU build id
src/agent/entry_hotpatch.*      Linux/x86-64 entry rewriter
src/cube_widget.*               OpenGL cube and execution seams
snippets/                       native runtime code examples and scene modifications
snippets/scene_toggle.h         runtime-added Cube menu entry shared by the scene snippets
snippets/cube_mesh.h            vertex-buffer claim and overlap rules for the scene snippets
snippets/audio_pulse.cpp        drives the cube and a rig around it from system audio
snippets/audio_analysis.h       capture, FFT and onset detection; depends on PulseAudio
                                and the standard library only, so unlike cube_mesh.h and
                                scene_toggle.h beside it, it knows nothing of Qt, the
                                agent ABI, or this demo
patches/                        function replacement examples
tests/                          hotpatch_selftest.cpp and the Python protocol,
                                agentctl and compile-oracle tests
tools/agentctl.py               protocol client, including snippet reload
tools/compile_snippet.py        compile-database build oracle
tools/read_build_id.py          reads the GNU build id modules are stamped with
tools/jit_snippet.py            compile + load + execute pipeline
tools/jit_patch.py              compile + load + hot-activate pipeline
tools/smoke_test.py             end-to-end GUI test
scripts/validate.sh             O3/LTO/CET and build-oracle validation
```
