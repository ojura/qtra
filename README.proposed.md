# Qt Runtime Agent / Hotpatch Cube

A Linux/GCC/Qt 6 prototype for controlling and extending a running optimized C++ application from a local client.

The demo is a `QMainWindow` with a continuously rotating OpenGL cube. An in-process `RuntimeAgent` listens on a user-only Unix-domain socket. A client can inspect and operate Qt objects, load project-native C++ modules, run code on selected Qt execution contexts, and replace a prepared function without adding a dispatch pointer to the application.

This is an owner-controlled local-process experiment. It is not a production debugger and it is not a security boundary. Native snippets and replacement functions run inside the application:

> Correct native code can do almost anything. Incorrect native code can corrupt or crash the process.

## Contents

- [Supported environment and fixed assumptions](#supported-environment-and-fixed-assumptions)
- [Five-minute quick start](#five-minute-quick-start)
- [What the build produces](#what-the-build-produces)
- [Crash course: how the tool works](#crash-course-how-the-tool-works)
- [Choose an interaction path](#choose-an-interaction-path)
- [Build and validation](#build-and-validation)
- [Run the application](#run-the-application)
- [Semantic Qt control](#semantic-qt-control)
- [Runtime-native snippets](#runtime-native-snippets)
- [Function replacement](#function-replacement)
- [Explicitly unsafe operations](#explicitly-unsafe-operations)
- [Troubleshooting](#troubleshooting)
- [Repository map](#repository-map)

## Supported environment and fixed assumptions

The prepared-entry patch backend has a deliberately narrow environment:

| requirement | value |
|---|---|
| operating system | Linux |
| architecture | x86-64 |
| compiler | GCC |
| application framework | Qt 6.4 or later |
| graphics | OpenGL 3.3 core profile |
| build system | CMake 3.21 or later with Ninja |
| scripting | Python 3 |
| function-entry preparation | `-fpatchable-function-entry`, all-NOP reserved area |

The three supplied CMake presets enable `PATCH_READY`. They prepare the demo target and generate the evidence used to decide whether an entry replacement reaches every call.

Keep the selected build directory present while its application is running. The application reads its coverage manifest from that compiled-in build directory. Moving only the executable or deleting the build directory makes first activation refuse.

Successful native modules remain loaded for the life of the process. A callback or an in-flight replacement may still execute code or read static state from an older module.

Raw memory access and deliberate crash commands require `--unsafe-agent`. Semantic control, snippets, and function replacement do not.

## Five-minute quick start

Run every command from the repository root.

### 1. Install dependencies

On Debian or Ubuntu:

```bash
sudo ./scripts/install_deps_debian.sh
```

The script installs GCC, CMake, Ninja, Python, Qt 6, Mesa, and Xvfb. The optional audio visualizer also needs PulseAudio development files:

```bash
sudo apt-get install -y --no-install-recommends libpulse-dev
```

A system Qt installation normally needs no extra configuration. For a Qt installation outside CMake's normal prefixes, set:

```bash
export CMAKE_PREFIX_PATH=/path/to/Qt/6.9.3/gcc_64
```

### 2. Build the optimized application

```bash
./scripts/build.sh release
```

This configures `build/release`, builds the application and modules, and runs the tests for that preset.

### 3. Start the application in terminal 1

```bash
./scripts/run_xvfb.sh
```

If `DISPLAY` is set, the wrapper uses that display and the cube is visible. If `DISPLAY` is unset, it starts Xvfb and forces Mesa software rendering.

The application prints a line like:

```text
Runtime agent listening on /tmp/qt-runtime-cube-1000.sock
```

### 4. Verify the connection in terminal 2

```bash
python3 tools/agentctl.py call hello
python3 tools/agentctl.py call cube.state
```

Both commands should return JSON with `"ok": true`. `hello` includes the executable build ID. `cube.state` reports the animation state.

Change an ordinary application property through the semantic API:

```bash
python3 tools/agentctl.py call cube.speed \
  --params '{"degreesPerSecond":180}'
```

### 5. Run native C++ inside the process

```bash
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_inspect.so \
  --executor gui \
  --request '{"nudgeAngle":15,"setSpeed":120}'
```

The module includes the application's real `CubeWidget` header and accesses its private state. The result names what it read and changed.

### 6. Replace the animation step function

```bash
python3 tools/agentctl.py patch build/release/cube_patch_wobble.so
python3 tools/agentctl.py call patch.status
```

The cube should wobble. `patch.status` should report:

```json
{"entryState":"replacement"}
```

The complete response also reports the gateway slot, coverage decision, write-admission policy, and observed thread count.

Return to the built-in function:

```bash
python3 tools/agentctl.py call patch.rollback
python3 tools/agentctl.py call patch.status
```

The state should be `original`. The gateway remains installed; its slot points at the function's continuation.

### 7. Stop the application

```bash
python3 tools/agentctl.py call process.quit
```

`Ctrl-C` in terminal 1 also ends the process.

## What the build produces

The `release` preset writes its artifacts under `build/release`:

| artifact | purpose |
|---|---|
| `qt_runtime_cube` | Qt/OpenGL demo with the in-process agent |
| `agent_snippet_*.so` | ready-made native snippet modules |
| `cube_patch_wobble.so` | sample replacement with a wobbling step function |
| `cube_patch_reverse.so` | sample replacement that reverses the step |
| `coverage-manifest.json` | build-time decision for replacing `cube_step_builtin` |
| `compile_commands.json` | compiler context used by the build oracle |
| `hotpatch_selftest` | optimized direct-call patch test |

The audio module is absent when `libpulse-simple` development files are unavailable. The rest of the build still succeeds.

## Crash course: how the tool works

### Terms

| term | meaning |
|---|---|
| client | `agentctl.py`, another local program, or an external operator speaking JSON-lines |
| `RuntimeAgent` | the in-process server that owns the socket, protocol, executors, events, and host ABI |
| host | the running application and the services it gives a native module |
| snippet | an ELF shared object exporting `runtime_agent_snippet_init()` |
| executor | the Qt context used to run a snippet: `gui`, `object`, or `render` |
| patch module | a shared object exporting a replacement descriptor for the demo step function |
| patch site | the resolved function entry and its prepared byte area |
| gateway | the permanent entry code that jumps through an atomic slot |
| continuation | the address that resumes the original function after the gateway |
| binding | one module's claim that a replacement may be selected |
| generation | a retained replacement and its owner; released generations remain mapped |
| build oracle | tooling that derives a module compiler command from `compile_commands.json` |
| coverage manifest | build-time evidence about aliases, clones, preparation, build identity, and caller execution domain |

### Runtime paths

```text
                         local client
                              |
                   JSON-lines over AF_UNIX
                              |
                  RuntimeAgent in the process
             +----------------+----------------+
             |                |                |
       semantic Qt       native modules      patch adapter
       operations        and executors            |
             |           gui/object/render    PatchRegistry
             |                                  |
       QObject tree                         PatchManager
                                                |
                                      gateway + atomic slot
                                                |
                                      cube_step_builtin or
                                         a replacement
```

The semantic path uses Qt metadata and ordinary application actions. The native path loads machine code compiled with the target project's context. The patch path changes where calls to one prepared function go.

### Build-time paths

```text
compile_commands.json + source
              |
   tools/compile_snippet.py
              |
      project-native DSO

final ELF + exact owning object + GCC clone dump + preparation flags
              |
     tools/analyze_coverage.py
              |
      coverage-manifest.json
              |
       runtime admission
```

The build oracle answers how to compile compatible code. The coverage analyzer answers whether changing one function entry reaches every relevant call.

### Snippet lifecycle

```text
load DSO -> run on executor -> optional persistent effect -> release effect
    |                                                   |
    +---------------- module remains resident ----------+
```

A snippet may connect signals, add actions, install event filters, retain static data, or bind a replacement. Its declared `release` function gives the driver a way to undo those effects. The DSO itself stays loaded.

### Patch lifecycle

```text
read build evidence
        |
check replacement effect and target identity
        |
resolve prepared site and replacement reachability
        |
first use only: admit live text write and install gateway
        |
bind generation by atomic slot store
        |
unbind -> select newest live predecessor or continuation
```

Gateway installation changes instruction bytes and needs a quiescent execution context. Binding and unbinding after installation change one aligned pointer slot.

A failed gateway installation can leave `recovery-required`. The patch record retains the original bytes, writer, quiescence lease, patch site, and the admission that covered that write. Recovery restores the entry bytes. This is separate from normal rollback, which leaves the gateway installed and selects the continuation.

## Choose an interaction path

| goal | path | command | executor or thread | needs `--unsafe-agent` |
|---|---|---|---|---|
| inspect Qt objects and properties | semantic protocol | `agentctl.py call` | GUI socket handler | no |
| invoke project-native C++ | snippet | `agentctl.py snippet` | `gui`, `object`, or `render` | no |
| compile a one-off native module | build oracle | `jit_snippet.py` | chosen executor | no |
| replace a prepared function | patch binding | `agentctl.py patch` or `jit_patch.py` | target's permitted control thread | no |
| read or write arbitrary mapped memory | unsafe protocol | `unsafe.memory.*` | request handler | yes |
| verify crash collection | unsafe protocol | `unsafe.crash` | request handler | yes |

Prefer semantic commands when Qt already describes the operation. Use a native snippet when the state or operation has no semantic API. Use function replacement when the application recomputes a value through a function on every tick and writes elsewhere do not persist.

## Build and validation

### One optimized build

```bash
./scripts/build.sh release
```

Equivalent commands:

```bash
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release --output-on-failure
```

### LTO build

```bash
./scripts/build.sh release-lto
```

The application uses LTO while the prepared target's translation unit uses `-fno-lto`. This keeps callers from seeing the target body.

### CET/IBT build

```bash
./scripts/build.sh release-cet
```

This enables `-fcf-protection=full`. The tests verify that the canonical `ENDBR64` remains intact and that an indirect replacement also starts with `ENDBR64`.

### Full validation

```bash
./scripts/validate.sh
```

This builds and tests `release`, `release-lto`, and `release-cet`, then rebuilds patch modules through each compile database and exercises them.

Add a GUI smoke session:

```bash
./scripts/validate.sh --with-gui
```

See [`docs/validation.md`](docs/validation.md) for exact test names, disassembly checks, and build-oracle validation commands.

## Run the application

### Existing display

```bash
./build/release/qt_runtime_cube
```

### Existing display or automatic Xvfb

```bash
./scripts/run_xvfb.sh
```

### Application options

| option | meaning |
|---|---|
| `--agent-socket PATH` | choose the Unix-domain socket |
| `--no-agent` | run only the visual demo |
| `--unsafe-agent` | enable raw memory and deliberate crash commands |

The default socket is:

```text
/tmp/qt-runtime-cube-<uid>.sock
```

Set `QT_RUNTIME_AGENT_SOCKET` or pass `tools/agentctl.py --socket PATH` when using another path.

The protocol uses one compact JSON object per line:

Request:

```json
{"id":1,"command":"cube.state","params":{}}
```

Success:

```json
{"id":1,"ok":true,"result":{"running":true}}
```

Event:

```json
{"event":"operation.finished","sequence":"31","monotonicNs":"918221044","data":{}}
```

See [`docs/protocol.md`](docs/protocol.md) for the complete command and event inventory.

## Semantic Qt control

The agent can discover QObjects, inspect properties, invoke zero-argument methods, trigger actions, click widgets, capture frames, and subscribe to events.

```bash
python3 tools/agentctl.py call object.tree \
  --params '{"maxDepth":3}'

python3 tools/agentctl.py call object.describe \
  --params '{"objectName":"cubeView","includeValues":true}'

python3 tools/agentctl.py call object.set \
  --params '{"objectName":"cubeView","property":"angularVelocity","value":180}'

python3 tools/agentctl.py call action.trigger \
  --params '{"objectName":"actionWireframe"}'
```

Stream selected events for one minute:

```bash
python3 tools/agentctl.py events \
  --prefix operation. --prefix patch. --seconds 60
```

Read retained events after sequence 100:

```bash
python3 tools/agentctl.py history --after 100 --prefix operation.
```

The server retains the newest 1,024 events. `--seconds` controls the total event-stream duration. The global client option `--timeout` controls one read and appears before the subcommand:

```bash
python3 tools/agentctl.py --timeout 20 events --all --seconds 60
```

## Runtime-native snippets

### ABI entry point

A snippet is an ordinary ELF shared object exporting one initializer:

```cpp
extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet*
runtime_agent_snippet_init();
```

Its descriptor names `run` and optional `release` functions. Both receive `RuntimeAgentHost`, which provides:

- logging and structured events;
- request JSON and completion or failure reporting;
- QObject and symbol lookup;
- patch binding;
- the opaque byte stash;
- monotonic time.

The loader accepts an exact ABI layout. Rebuild snippets after `include/agent/agent_abi.h` changes.

### Executors

| executor | where code runs | use it for |
|---|---|---|
| `gui` | main-window event loop | Qt widgets, actions, application-wide filters |
| `object` | selected QObject's event loop | thread-affine object work |
| `render` | inside `CubeWidget::paintGL()` with the GL context current | OpenGL inspection and drawing |

Examples:

```bash
python3 tools/agentctl.py snippet \
  build/release/agent_snippet_about_signature.so \
  --executor gui --request '{"line":"runtime edit"}'

python3 tools/agentctl.py snippet \
  build/release/agent_snippet_render_probe.so \
  --executor render --request '{}'

python3 tools/agentctl.py snippet SOME_SNIPPET.so \
  --executor object --target-name cubeView --request '{}'
```

`render` work is serviced by `paintGL()`. A 100 ms watchdog uses `grabFramebuffer()` when compositor behavior leaves queued work waiting.

### Ready-made modules

| module | executor | effect |
|---|---|---|
| `agent_snippet_inspect` | `gui` | reads and changes private `CubeWidget` state |
| `agent_snippet_observer` | `gui` | installs a persistent observer |
| `agent_snippet_render_probe` | `render` | reports GL context and pipeline state |
| `agent_snippet_about_signature` | `gui` | appends a line to the About dialog |
| `agent_snippet_orbit_sphere` | `render` | draws an orbiting sphere |
| `agent_snippet_catmull_clark` | `render` | replaces the cube mesh with a subdivision mesh |
| `agent_snippet_mobius_ring` | `render` | draws a rotating Mobius strip |
| `agent_snippet_pillow_cube` | `render` | replaces the cube with padded panels |
| `agent_snippet_jack_in_the_box` | `render` | opens one cube face and draws a sprung jack |
| `agent_snippet_audio_pulse` | `render` | drives animation and graphics from PulseAudio |

The six scene modules add checkable actions to the Cube menu. Overlay modules compose. Mesh replacements use stash claims and value checks to refuse overlapping modifications that cannot be restored safely.

Re-run a loaded module by ID:

```bash
python3 tools/agentctl.py call snippet.run \
  --params '{"moduleId":2,"executor":"render","request":{"level":4}}'
```

Release its persistent effect:

```bash
python3 tools/agentctl.py call snippet.release \
  --params '{"moduleId":2}'
```

The agent records the executor and target used by successful runs and attempts. A release uses that record unless the request supplies an override.

### Build identity

A native module that reads application types directly compiles member offsets into its code. Every project-built module is stamped with the executable's GNU build ID. The loader compares that stamp with the running process.

| module stamp | result |
|---|---|
| matches running build | load |
| names another build | refuse and report both IDs |
| absent | load and report unchecked offsets |

`hello` reports the running build ID. Rebuild a refused module against the selected build directory.

### Compile a one-off snippet

Compile only:

```bash
python3 tools/compile_snippet.py \
  --compile-db build/release/compile_commands.json \
  --context src/cube_widget.cpp \
  --source /tmp/my_probe.cpp \
  --output build/release/runtime-snippets/my_probe.so
```

Compile, load, run, and wait for completion:

```bash
python3 tools/jit_snippet.py /tmp/my_probe.cpp \
  --compile-db build/release/compile_commands.json \
  --context src/cube_widget.cpp \
  --executor gui \
  --request '{"anything":"the snippet expects"}'
```

The compiler transformer preserves compiler, working directory, include order, definitions, language dialect, architecture options, forced includes, and ABI settings. It replaces input, output, dependency, optimization, LTO, profile, and PIE options with shared-object settings.

### Compile and activate a replacement

The application and self-test both compile `src/cube_step.cpp`. Name the application object in the compile context:

```bash
python3 tools/jit_patch.py patches/wobble_patch.cpp \
  --compile-db build/release/compile_commands.json \
  --context qt_runtime_cube.dir/src/cube_step.cpp.o \
  --optimization O3
```

Every JIT output uses a fresh path so `dlopen()` cannot return an older resident object for the same canonical filename.

### Reload an edited snippet

`dlopen()` returns the resident module when asked for an already-loaded path. Build the target, then use `reload` to copy it to a fresh content-addressed path, release the outgoing effect, and run the new module:

```bash
cmake --build --preset release --target agent_snippet_jack_in_the_box

python3 tools/agentctl.py reload \
  build/release/agent_snippet_jack_in_the_box.so \
  --request '{}'
```

A failed release stops the handover unless `--force` is present. The outgoing DSO remains resident even after a successful release.

### Opaque byte stash

A module that overwrites host data can deposit the original bytes in the host:

```cpp
host->stash_put(host->agent_context,
                "jack_in_the_box/face-front",
                vertices.data(),
                byteCount,
                1);
```

The host stores bytes under flat caller-chosen keys. It records size, time, module ID, and module name without interpreting the contents.

```bash
python3 tools/agentctl.py call stash.list
python3 tools/agentctl.py call stash.get \
  --params '{"key":"jack_in_the_box/face-front"}'
python3 tools/agentctl.py call stash.drop \
  --params '{"key":"jack_in_the_box/face-front"}'
```

A module replays saved bytes only after checking that its own displacement is still present. The driver drops a record after observing a successful restore.

## Function replacement

The demo application calls `cube_step_builtin` directly once per animation tick:

```cpp
const CubeStepOutput output = cube_step_builtin(&input);
```

The application holds no dispatch pointer for the agent. The build prepares the function; the patch system resolves its entry and installs the redirection.

### Preparing the target

CMake applies these options to the target translation unit:

```text
-fpatchable-function-entry=20,0
-fno-lto
-fno-ipa-icf
```

The source carries no patch-specific attributes.

- `-fpatchable-function-entry=20,0` reserves the entry area.
- `-fno-lto` keeps callers in other translation units from seeing the body.
- `-fno-ipa-icf` prevents folding with another function.

`PATCH_READY` also keeps split DWARF and GCC IPA clone dumps for the coverage analysis.

### Coverage and identity

Changing an entry reaches calls that arrive at that entry. Inlined copies, specialized clones, and same-address aliases require separate accounting.

`tools/analyze_coverage.py` combines:

- final ELF symbols and aliases;
- the target's prepared entry;
- the exact object that owns the target;
- that object's GCC IPA clone dump;
- preparation flags;
- the executable build ID;
- a declared or proved caller execution domain.

The result is `build/release/coverage-manifest.json` with coverage `complete`, `incomplete`, or `unknown`.

Activation first verifies that the manifest names this executable build and this target. Identity failures cannot be accepted. The `acceptIncompleteCoverage` flag applies only to an incomplete or unknown replacement effect.

Replacement coverage and live text writing answer different questions:

- **Replacement effect:** Does selecting this replacement reach every intended call?
- **Live text write:** May the process change instruction bytes at this moment?

The first activation needs both. Later bindings use the existing gateway and need only the replacement-effect decision.

### Gateway layout

The first installation writes this into the prepared area:

```text
movabs r11, <slot address>     10 bytes
jmp    qword ptr [r11]          3 bytes
endbr64                         4 bytes
nop ...                         remaining prepared bytes
```

The slot initially points at the continuation `ENDBR64`, so calls still run the original function.

Under CET/IBT, GCC's canonical entry begins with `ENDBR64`. The patch system leaves it intact and writes the gateway at `entry + 4`. Indirect replacement targets must also begin with `ENDBR64`.

The gateway's slot storage belongs to the process-lifetime `PatchRegistry`. The entry embeds that slot address, so the storage is never reclaimed.

### Installing and selecting

Gateway installation is the only patch operation that writes instruction bytes. It requires a `LiveTextWriteAdmission` and a quiescer.

In the cube demo, every caller runs on the widget's owning thread. The protocol and host binding routes also run there, between animation ticks, with the target absent from the stack. The build records that execution-domain claim and the runtime verifies it before installation.

After installation:

- binding a replacement appends a generation and stores its address in the slot;
- unbinding a selected generation stores the newest live predecessor or the continuation;
- unbinding a generation that is not selected leaves the slot unchanged.

An aligned x86-64 pointer store is atomic. A concurrent gateway load sees the old destination or the new one.

### Entry states

| `entryState` | meaning |
|---|---|
| `pristine` | the reserved NOP area is untouched |
| `original` | the gateway is installed and the slot names the continuation |
| `replacement` | the gateway is installed and the slot names a replacement |
| `recovery-required` | installation changed bytes and did not complete cleanly |

Normal rollback deselects the active binding and leaves the gateway installed:

```bash
python3 tools/agentctl.py call patch.rollback
```

Recovery is a different operation. It restores saved entry bytes after a failed installation. The recovery record keeps the exact admission, writer, original bytes, site, and quiescence lease associated with that write.

### Binding from a native module

A module can bind a replacement through the stable host ABI:

```cpp
RuntimeAgentPatchBinding bound{};
const std::int32_t result = host->patch_bind(
    host->agent_context,
    reinterpret_cast<void*>(&cube_step_builtin),
    reinterpret_cast<void*>(&myStep),
    0,
    &bound);
```

On success, `bound` contains:

- `id`: binding ID;
- `original`: continuation address for calling the built-in function;
- `previous`: the destination displaced by this binding.

Release it with:

```cpp
host->patch_unbind(host->agent_context, bound.id);
```

Ownership comes from `agent_context`. A module cannot release another module's binding. Owner identities and patch state outlive any one `ModuleManager` borrower.

Call both operations from the executor used by the target. The cube adapter refuses host binding calls from another thread.

### In-flight calls and lifetime

A slot store affects entries that load the slot after they observe the store. A call already running continues in the generation it entered.

Replacement code remains mapped for the process lifetime. State used by an old invocation must also remain valid until the relevant execution context is quiet. The patch system does not drain in-flight calls and does not reclaim released generations.

### Prepared-entry limits

The backend supports Linux/x86-64 prepared all-NOP entries. It does not relocate arbitrary prologues, rewrite every call site, or use GOT/PLT replacement as its primary path.

Installing a gateway changes several bytes and is not atomic. A target outside the demo's declared same-thread domain needs a policy that accounts for every thread before that write.

## Explicitly unsafe operations

Start the application with raw memory and deliberate crash commands enabled:

```bash
./scripts/run_xvfb.sh --unsafe-agent
```

Resolve a dynamic symbol:

```bash
python3 tools/agentctl.py call symbol.resolve \
  --params '{"name":"cube_step_builtin"}'
```

Read or write mapped memory:

```bash
python3 tools/agentctl.py call unsafe.memory.read \
  --params '{"address":"0x...","size":32}'

python3 tools/agentctl.py call unsafe.memory.write \
  --params '{"address":"0x...","base64":"..."}'
```

These commands use `process_vm_readv` and `process_vm_writev` against the current process. A request can corrupt any writable mapped state permitted by the kernel. Transfers are capped at 65,536 bytes.

`unsafe.crash` intentionally dereferences null to test crash collection.

## Troubleshooting

### `agentctl.py` cannot connect

Check that the application printed its listening path. The application and client default to `/tmp/qt-runtime-cube-<uid>.sock`.

For a custom path, use the same value on both sides:

```bash
./build/release/qt_runtime_cube --agent-socket /tmp/my-agent.sock
python3 tools/agentctl.py --socket /tmp/my-agent.sock call hello
```

### CMake cannot find Qt

Install the distribution Qt 6 development packages, or set `CMAKE_PREFIX_PATH` for a non-system Qt installation.

### No display or OpenGL context

Use:

```bash
./scripts/run_xvfb.sh
```

The dependency script installs Xvfb and Mesa software rendering packages.

### Coverage manifest is missing

Build with one of the supplied presets and keep that build directory present:

```bash
./scripts/build.sh release
```

`PATCH_READY` is enabled in `release`, `release-lto`, and `release-cet`.

### A module names another build ID

Rebuild the module against the running executable's build directory. `hello` reports the process build ID.

### Compile context is ambiguous

Use the object path named by the error. For the application patch target:

```text
qt_runtime_cube.dir/src/cube_step.cpp.o
```

### A snippet needs another executor

Qt widget work belongs on `gui`. Thread-affine object work belongs on `object`. OpenGL work belongs on `render`.

### Reload runs old code

Use `agentctl.py reload` or a fresh output path. `dlopen()` reuses a resident object for the same canonical pathname.

### Audio module is absent

Install `libpulse-dev`, then rebuild. CMake skips only `agent_snippet_audio_pulse` when PulseAudio development files are unavailable.

### Patch status is unclear

```bash
python3 tools/agentctl.py call patch.status
```

Read `entryState`, `coverage`, `quiescedBy`, and `threadsAtInstall`. `original` means the permanent gateway is present and selects the built-in continuation.

## Repository map

```text
include/agent/agent_abi.h       stable native-module host ABI
include/demo/cube_step_abi.h    demo replacement function and descriptor ABI

docs/protocol.md                JSON command and event reference
docs/validation.md              test, disassembly, and oracle validation reference

src/agent/runtime_agent.*       socket server, commands, events, executors, host callbacks
src/agent/object_registry.*     QObject IDs, discovery, and reflection
src/agent/module_manager.*      DSO/snippet ownership and cube-specific patch adapter
src/agent/build_id.*            running executable GNU build ID
src/agent/coverage_manifest.*   runtime reading of build-time coverage evidence
src/agent/entry_hotpatch.*      mapped-text writer and gateway encoding
src/agent/patch_site.*          prepared site resolution and validation
src/agent/patch_manager.*       gateway installation, generations, selection, and recovery
src/agent/patch_registry.*      process-lifetime ownership of per-entry patch state
src/agent/gateway_record.h      gateway slot and continuation storage
src/agent/write_admission.h     immutable record of what admitted a live text write
src/agent/quiescence*           policies and leases used during text writes

src/cube_widget.*               OpenGL cube and GUI/render execution paths
src/cube_step.cpp               ordinary direct-call function prepared by CMake

snippets/                       native runtime code examples
snippets/scene_toggle.h         shared runtime-added Cube menu action
snippets/cube_mesh.h            mesh displacement claims and overlap checks
snippets/audio_analysis.h       PulseAudio capture, FFT, and onset detection
patches/                        function replacement examples

tools/agentctl.py               protocol client, event client, snippet reload
tools/compile_snippet.py        compile-database build oracle
tools/jit_snippet.py            compile, load, execute pipeline
tools/jit_patch.py              compile, load, activate replacement pipeline
tools/analyze_coverage.py       build-time replacement coverage analysis
tools/read_build_id.py          GNU build-ID reader
tools/smoke_test.py             end-to-end GUI test

scripts/build.sh                one-preset build and test
scripts/run_xvfb.sh             display selection and Mesa/Xvfb launcher
scripts/validate.sh             O3, LTO, CET, oracle, and optional GUI validation

tests/                          native patch and Python protocol/tool tests
```

## Further reference

- [`docs/protocol.md`](docs/protocol.md): every command, response, error, and event
- [`docs/validation.md`](docs/validation.md): exact validation procedure
- [`include/agent/agent_abi.h`](include/agent/agent_abi.h): native module ABI
- [`patches/`](patches/): replacement examples
- [`snippets/`](snippets/): native runtime examples
