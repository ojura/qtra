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

Three prebuilt snippets change what the widget draws. Each installs a direct
connection to `CubeWidget::frameRendered`, which is emitted at the end of
`paintGL()` while the context is still current and the depth buffer still holds
the cube, so a hook there gets correct occlusion without touching `paintGL()`.
All three read private `CubeWidget` state for the frame's angle, scale, tint,
elapsed time, and projection, and all three are compiled with
`-fno-access-control`.

```bash
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_orbit_sphere.so --executor render --request '{}'
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_catmull_clark.so --executor render --request '{}'
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_mobius_ring.so --executor render --request '{}'
```

| module | what it does | parameters |
| --- | --- | --- |
| `agent_snippet_orbit_sphere` | shaded sphere on a tilted orbit | none |
| `agent_snippet_catmull_clark` | replaces the cube with its Catmull-Clark subdivision surface, keeping the six face colors | `level` 0–5, default 2 |
| `agent_snippet_mobius_ring` | rotating Mobius strip, translucent surface and opaque rim | `radius`, `width`, `tilt`, `spin`, `alpha`, `edgeInset` |

Each accepts `{"restore": true}` to take itself back off. The Catmull-Clark one
has to suppress the widget's own draw, because `paintGL()` calls
`glDrawArrays(GL_TRIANGLES, 0, 36)` with the count baked into compiled code and
a subdivided mesh cannot go through that buffer. It keeps the widget's 36
original vertices, overwrites them with zeros so that draw rasterizes only
degenerate triangles, and puts them back on restore.

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
  Qt affinity, or application invariants, and would increase the blast radius.

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
patches/                        function replacement examples
tools/agentctl.py               protocol client
tools/compile_snippet.py        compile-database build oracle
tools/jit_snippet.py            compile + load + execute pipeline
tools/jit_patch.py              compile + load + hot-activate pipeline
tools/smoke_test.py             end-to-end GUI test
scripts/validate.sh             O3/LTO/CET and build-oracle validation
```
