# Local validation result — 2026-08-17

This record describes what was actually built and executed in the supplied
Debian 13 container. It intentionally separates the Qt-independent native core
from the Qt GUI target.

## Passed from clean out-of-tree builds

Toolchain:

```text
GCC 14.2.0
CMake 3.31.6
Ninja 1.12.1
Python 3.13
Linux x86-64
```

Three independent Release builds were configured from an empty build
directory:

| Build | Native tests | Result |
|---|---:|---|
| ordinary `-O3` | 3/3 | pass |
| `-O3` plus GCC LTO | 3/3 | pass |
| `-O3` plus CET/IBT (`-fcf-protection=full`) | 3/3 | pass |

Each build exercised both patch modules and verified the full sequence:

1. call the optimized built-in function;
2. install an x86-64 entry redirect;
3. observe replacement behavior;
4. roll back the original bytes;
5. observe the original behavior again.

The CET build begins with `ENDBR64`; the patcher preserves it and redirects at
the prepared NOP area after the landing pad. Both ordinary and CET builds emit
a `__patchable_function_entries` section.

The build-oracle tool also derived a fresh GCC shared-object command from the
clean `compile_commands.json`, compiled `patches/wobble_patch.cpp` at `-O3`, and
that newly generated module passed redirect and rollback.

The Python controller/compiler suite passed 9/9 tests. Python byte-compilation,
all shell-script syntax checks, and Git whitespace validation also passed.

## Not executed in this container

The Qt 6 development packages are absent and the container shell cannot resolve
or reach the configured Debian mirrors. Therefore the `qt_runtime_cube` target,
its Qt-native snippets, the Xvfb OpenGL smoke test, and framebuffer capture were
not compiled or run here.

The GUI source was reviewed for Qt meta-object constraints and obvious include
issues. In particular, `frameIndex` now has a dedicated one-argument NOTIFY
signal rather than using the two-argument high-rate `frameRendered` signal.
This review is not a substitute for compiling against Qt 6.

Run the complete validation on a Qt-capable Linux host with:

```bash
./scripts/install_deps_debian.sh
./scripts/validate.sh --with-gui
```
