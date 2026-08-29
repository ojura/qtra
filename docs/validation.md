# Validation record

Three builds, differing in the codegen the patcher has to survive:

```bash
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release --output-on-failure
```

Expected tests:

```text
raw-entry-hotpatch-wobble
raw-entry-hotpatch-reverse
coverage-admission-order
python-tool-tests
```

The same suite with GCC interprocedural optimization enabled, which is what
`-fno-lto` on the target has to hold out against:

```bash
cmake --preset release-lto
cmake --build --preset release-lto --parallel
ctest --preset release-lto --output-on-failure
```

The CET/IBT layout has its own build:

```bash
cmake --preset release-cet
cmake --build --preset release-cet --parallel
ctest --preset release-cet --output-on-failure
```

Its target disassembly begins with `endbr64`, followed by 16 NOP bytes. The
patcher leaves `endbr64` intact and changes the bytes beginning at `entry + 4`.
The tests also reject a CET-prepared target paired with a replacement lacking
an `ENDBR64` landing pad.

Qt 6 is required for all three. `cmake` finds it from the `CMAKE_PREFIX_PATH`
environment variable, which the presets do not set:

```bash
export CMAKE_PREFIX_PATH=/path/to/Qt/6.9.3/gcc_64
```

`./scripts/validate.sh` runs all three, then rebuilds the patch module through
the build oracle against each one's compile database and exercises it. Add
`--with-gui` to drive the running application afterwards.

The Release compile database should show `-O3` for `src/cube_step.cpp`.

Inspect the prepared entry:

```bash
objdump -d -M intel build/release/hotpatch_selftest \
  | sed -n '/<cube_step_builtin>:/,+20p'
readelf -SW build/release/hotpatch_selftest \
  | grep __patchable_function_entries
```

Build a fresh patch module using the build oracle rather than CMake's patch
target, then exercise redirect and rollback. The application and the self-test
each compile `src/cube_step.cpp`, so the context names the object rather than
the source:

```bash
python3 tools/compile_snippet.py \
  --compile-db build/release/compile_commands.json \
  --context hotpatch_selftest.dir/src/cube_step.cpp.o \
  --source patches/wobble_patch.cpp \
  --output build/release/oracle_wobble.so \
  --optimization O3 --respect-access-control

./build/release/hotpatch_selftest build/release/oracle_wobble.so
```

The one-command patch compiler/activator has a compile-only validation mode:

```bash
python3 tools/jit_patch.py patches/reverse_patch.cpp \
  --compile-db build/release/compile_commands.json \
  --context hotpatch_selftest.dir/src/cube_step.cpp.o \
  --output build/release/jit_reverse.so \
  --compile-only --respect-access-control

./build/release/hotpatch_selftest build/release/jit_reverse.so
```

The GUI validation command is:

```bash
python3 tools/smoke_test.py --build-dir build/release --keep-runtime
```
