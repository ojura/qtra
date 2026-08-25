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

## Validation status

The Qt-independent part has been built and executed in this repository with GCC
14 in a clean Release configuration:

- the host function is compiled at `-O3`;
- disassembly shows the requested 16-byte GCC patchable entry region;
- both supplied patch DSOs redirect the function and rollback successfully;
- the same redirect/rollback tests also pass with GCC LTO enabled;
- an Intel CET/IBT build preserves `ENDBR64`, patches the following NOP sled,
  rejects replacement entry points without `ENDBR64`, and passes rollback;
- the build-oracle tool generated a fresh `-O3` patch DSO from
  `compile_commands.json`, and that generated DSO passed the same test;
- Python tests cover compilation-command transformation, retained-event replay,
  quiet event streams, and socket protocol behavior.

The current execution container does not contain the Qt 6 development packages,
and its shell cannot reach Debian package mirrors. Consequently the GUI target
could not be compiled or launched here. The GUI source, build scripts, Xvfb
runner, controller, and end-to-end smoke test are included for a Qt-capable
Linux environment.

The exact clean-build results are recorded in [`docs/validation-result.md`](docs/validation-result.md).

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

The earlier attempt inside the supplied container used the same Qt packages but
did not complete because the container could not resolve/reach Debian mirrors.
Qt was therefore not successfully installed there.

Required application components are Qt 6 Core, Gui, Widgets, Network,
Concurrent, OpenGL, and OpenGLWidgets. The CMake project currently requires Qt
6.4 or later.

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
python3 tools/agentctl.py events --prefix operation. --prefix patch. --timeout 60
python3 tools/agentctl.py history --after 100 --prefix operation.
```

The protocol is one compact JSON object per line. Requests have `id`, `command`,
and `params`. Replies have matching `id`, `ok`, and either `result` or `error`.
Events have `event`, monotonically increasing `sequence`, `monotonicNs`, and
`data`.

See [`docs/protocol.md`](docs/protocol.md) for the command inventory.

## Runtime-native snippets

A snippet is an ordinary ELF shared object with one stable exported initializer:

```cpp
extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1*
runtime_agent_snippet_init_v1();
```

Its descriptor points to a function receiving `RuntimeAgentHostV1`. The host ABI
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

Four prebuilt snippets change what the widget draws. Each installs a direct
connection to `CubeWidget::frameRendered`, which is emitted at the end of
`paintGL()` while the context is still current and the depth buffer still holds
the cube, so a hook there gets correct occlusion without touching `paintGL()`.
All four read private `CubeWidget` state for the frame's angle, scale, tint,
elapsed time, and projection, and all four are compiled with
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
```

| module | what it does | parameters |
| --- | --- | --- |
| `agent_snippet_orbit_sphere` | shaded sphere on a tilted orbit | none |
| `agent_snippet_catmull_clark` | replaces the cube with its Catmull-Clark subdivision surface, keeping the six face colors | `level` 0–5, default 2 |
| `agent_snippet_pillow_cube` | turns each side into a stuffed pillow panel with sewn edges and stitching | `puff`, `roundness`, `tuck`, `resolution`, `seam`, `stitches`, `fabric` |
| `agent_snippet_mobius_ring` | rotating Mobius strip, translucent surface and opaque rim | `radius`, `width`, `tilt`, `spin`, `alpha`, `edgeInset` |

### Switching them on and off

All four add a checkable entry to the application's Cube menu when the snippet
is loaded, under a separator that marks off everything added at runtime:

| entry | shortcut | object name |
| --- | --- | --- |
| Orbiting sphere | `Ctrl+Shift+O` | `actionOrbitSphere` |
| Catmull-Clark | `Ctrl+Shift+C` | `actionCatmullClark` |
| Mobius ring | `Ctrl+Shift+M` | `actionMobiusRing` |
| Pillow mode | `Ctrl+Shift+P` | `actionPillowMode` |

The application has no knowledge of these snippets and must not acquire any:
they are separate shared objects that may never be loaded at all. Each entry is
therefore created by the snippet once it is in the process, and its handler is
code inside that module — one more reason a loaded snippet is never unloaded.

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
`paintGL()` through `CubeWidget::enqueueRenderCallback()` rather than touching
GL state where it stands. Parameters survive being switched off: a snippet keeps
the values it was last given and reuses them when it comes back on.

A second copy of the same snippet loaded from a different path does not add a
second entry. It would carry the same name while driving different state, so the
module that got there first keeps the menu, and the later one is reachable only
through its own `moduleId`.

**Catmull-Clark and the pillow exclude each other.** Both replace the cube's own
mesh, and both do it by saving the widget's 36 original vertices and then
overwriting them with zeros. Installed one on top of the other, the second would
save the first one's zeros as if they were the cube, and restoring later would
put those zeros back and lose the cube for the life of the process. Switching
either one on therefore switches the other off first, through the menu entry
rather than a shared symbol: the two are separate shared objects with no way to
call each other, so the exclusion runs by unchecking the other's `QAction` and
letting the module that owns it do its own removal.

That handles the case where both entries exist. As a backstop for when they do
not — a second copy from a JIT path never gets one, since the name is taken —
each of the two also reads the widget's vertices before saving them and refuses
to install if they are already all zeros:

```text
the widget's vertices are already collapsed, so another mesh replacement owns
them; saving these zeros would lose the cube
```

The overlays are a different matter: the sphere and the ring only add draw
passes, so they compose with each other and with either mesh replacement.

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
ball rather than a cube. `fabric` fades the six saturated face colors toward a
linen tone; it defaults to 0, which keeps them exactly as the widget draws them.
Re-running the loaded module keeps any parameter the new request omits.

Re-run a loaded module rather than loading it again, so that the module's static
state is the one that gets updated:

```bash
python3 tools/agentctl.py call snippet.run \
  --params '{"moduleId":2,"executor":"render","request":{"level":4}}'
python3 tools/agentctl.py call snippet.run \
  --params '{"moduleId":3,"executor":"render","request":{"restore":true}}'
```

Loading a *different* path gives a separate module with its own copy of that
state, which for these means a second draw hook rather than a change to the
first. `tools/jit_snippet.py` writes a fresh path every time, precisely so
`dlopen()` cannot hand back an already loaded object, so recompiling one of
these and running it again does stack a second copy.

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

Replacing the About action's handler would have been the obvious approach, and
it is the wrong one: the wording lives in a lambda in `main_window.cpp`, a
snippet cannot read a string out of compiled code, and a replacement handler
would have to restate the application's own text and then go stale when the
application changes it.

The snippet installs an application-wide event filter instead. `QMessageBox`
sees `QEvent::Show` before it reaches the screen, so the application builds its
dialog exactly as it always does and the line is appended to whatever text that
box turned out to carry. Reading it back while the dialog was open gave:

```text
An optimized Qt 6/OpenGL process with semantic RPC, runtime-compiled C++
snippets, and two hotpatch modes.

claude was here
```

`{"restore": true}` removes the filter and reports how many dialogs it changed.

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
the *old* module, and reports success while doing it: a new module id comes
back, `snippet.run` completes, and nothing anywhere says the edit was ignored.
Measured on this repository, rebuilding with `springRingSegments` changed from
8 to 12 and loading the same path still reported the old `springTriangles` of
1920.

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
then runs the new one. The same edit through this path reported 3360 triangles
for `springRingSegments = 14`, in a process that was never restarted.

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
Nothing can be unloaded, so the new module stays resident and inert instead. Without a handover the new generation would install
alongside the old one, which for two `frameRendered` hooks means two copies
rather than a replacement.

`--handover-request` is still there for a module that declares no release. It
sends a request payload and cannot report whether anything acted on it, which is
the difference the declaration removes.

Generations are matched by the descriptor's `name_utf8`, which is the same
string in every build of one source, rather than by filename — reloads and
`jit_snippet.py` output do not share a filename stem. `--replace ID` picks the
outgoing module explicitly.

`jit_snippet.py` remains the tool for a source file with no CMake target. For
one that has a target, building it is not the slower option — an incremental
build measured 4.4 s against 4.1 s for the oracle's own compile — and the built
object is the artifact the project actually produces, compiled with the
project's command line rather than the oracle's reconstruction of it.

Two properties still hold across a reload:

- Nothing is unloaded, so every iteration leaves its predecessor resident.
- A menu entry created by `scene_toggle` is handed to the newest generation.
  The entry is rebound rather than duplicated, so the checkbox drives the code
  that is installed. This assumes the handover ran first.

Plain `agentctl.py snippet` on a path that is already loaded now says so on
stderr, rather than letting a stale load look like a rebuild that did not take.

Work queued for the `render` executor runs inside `paintGL()`, and `update()`
only asks for a repaint. A window the compositor has stopped sending frame
callbacks to — covered, unfocused, or minimised — may never provide one, and
queued work would then wait for as long as the window stays hidden while the
caller sees only its own request timing out. `CubeWidget` therefore starts a
100 ms single-shot watchdog whenever it queues render work, and a queue still
unserved when it fires is drained through `grabFramebuffer()`, which renders
whatever the compositor is doing. A visible window never reaches it. This lives
in the widget rather than in the client tools, so every caller gets it.

### Declaring how a module lets go

A snippet that installs something lasting can declare how to undo it, as a
fifth field in its descriptor:

```cpp
const RuntimeAgentSnippetV1 descriptor{
    RUNTIME_AGENT_ABI_V1, sizeof(RuntimeAgentSnippetV1), "...", &run, &release,
};
```

`release` receives a host and reports through `complete_json`/`fail` exactly as
`run` does, so a release that fails is an error a program can act on rather than
a payload nobody checked.

There is no compatibility path for older descriptors, and none is wanted here.
`RUNTIME_AGENT_ABI_V1` is bumped whenever either ABI struct changes shape, and
the loader refuses anything that does not match, so a module built against an
older layout fails to load instead of reading fields the host never wrote.
Rebuild the snippets when the header changes; a stale `.so` under
`runtime-snippets/` will be rejected rather than run.

A null `release` means the module declares it has nothing to undo. That is a
real answer and not a placeholder — of the nine snippets here, `inspect_cube`,
`install_observer` and `render_probe` genuinely install nothing that outlives
the call, and `install_observer` already said as much in its own result.

```bash
python3 tools/agentctl.py call snippet.release --params '{"moduleId":1}'
python3 tools/agentctl.py call module.list
```

No executor is needed. The agent records how each module was last run
*successfully* — the last success rather than the first attempt, since a snippet
needing a GL context fails under `gui` with "use executor=render" and is retried
— and runs the release there. `module.list` reports that record alongside
`declaresRelease`. Passing `executor` or `target` overrides it; a recorded
target that no longer exists is reported as `recorded_target_gone` rather than
quietly replaced.

Release must not report completion before its effects are applied, because a
handover is sequenced on that completion. The scene snippets refuse rather than
defer for this reason: releasing one under `gui` returns an error telling you to
use `render`, instead of queueing the teardown for the next frame and reporting
success while the cube is still collapsed.

What this does not do is verify the effects are gone. It raises "nobody can tell
whether the handover did anything" to "the handover ran and reported"; a buggy
release that leaves half its state behind passes exactly as before. The
declaration is also one bit per module, so a snippet that installed three things
cannot say it can undo two. The install-time defences — the scene snippets
refusing to save an already-collapsed vertex buffer — still matter for that
reason, because nothing distinguishes "nothing to release" from "the author
forgot".

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

Two rules follow from that meaning, and both are placement rather than
exhortation:

- **Deposit behind the guard that makes the install safe.** The jack already
  refuses to install onto an already-collapsed face, and the deposit sits after
  that check, so nothing can reach it with bytes the module has not just
  validated. There is no separate obligation on whoever writes the next snippet.
- **Replay only onto your own displacement.** Before writing the saved bytes
  back, the jack checks the face still holds the zeros it wrote. If something
  else changed it since, the bytes are left alone and the result says so. That
  check is what makes an entry which outlives its release safe to keep.

The stash holds the *only* copy — the jack keeps no private duplicate and its
own restore reads it back from there. That is deliberate: a saved copy nobody
reads is a copy nobody notices going wrong, whereas here the bytes another
module would rely on are the bytes this module's own restore needs. Drop the
entry while the face is still open and releasing reports that the face could not
be restored, rather than writing whatever happened to be in the buffer.

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
belongs to whoever is driving; and a restore that corrupts rather than restores
must not delete the only good copy as its final act.

This is not an undo log. The host stores bytes and has no idea what they mean,
which is the point — a host that had to replay effects would need a vocabulary
covering everything a snippet might do, and that contradicts snippets being able
to do almost anything.

Not everything needs saving. `cubeVertices` is `constexpr` in an anonymous
namespace, so it is absent from `.dynsym` and `symbol.resolve` misses it, but it
is still in `.symtab` and readable from the live process with
`unsafe.memory.read` at a load base derived from any resolvable symbol — and it
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

`cube_step_builtin_v1` is built as:

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
  --params '{"name":"cube_step_builtin_v1"}'
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
- Only successful DSO loads are retained. Failed loads are now closed.
- The semantic registry covers the main-window QObject tree and explicitly
  registered objects; non-QObject domain state needs generated/manual adapters.
- `object.invoke` currently handles only zero-argument methods.
- QObject property operations are executed by the GUI-thread socket handler.
  Worker-thread-affine objects should be accessed through a queued native
  snippet targeting that object.
- An optimized-away source variable may have no recoverable runtime value. The
  agent can use DWARF and assembly where possible, or hot-install a rebuilt probe
  that explicitly materializes the value on the next execution.
- A kernel module is unnecessary for this prototype. It would not solve C++ ABI,
  Qt affinity, or application invariants, and a bug in one takes down the whole
  machine instead of a single process.

## Repository map

```text
include/agent/agent_abi.h       stable snippet host ABI
include/demo/cube_step_abi.h    patch function/descriptor ABI
src/agent/runtime_agent.*       JSON socket and command dispatch
src/agent/object_registry.*     QObject IDs and reflection
src/agent/module_manager.*      dlopen, snippet, dispatch, entry patch state
src/agent/entry_hotpatch.*      Linux/x86-64 entry rewriter
src/cube_widget.*               OpenGL cube and execution seams
snippets/                       native runtime code examples and scene modifications
snippets/scene_toggle.h         runtime-added Cube menu entry shared by the scene snippets
patches/                        function replacement examples
tools/agentctl.py               protocol client, including snippet reload
tools/compile_snippet.py        compile-database build oracle
tools/jit_snippet.py            compile + load + execute pipeline
tools/jit_patch.py              compile + load + hot-activate pipeline
tools/smoke_test.py             end-to-end GUI test
scripts/validate.sh             O3/LTO/CET and build-oracle validation
```
