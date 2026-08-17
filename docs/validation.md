# Validation record

The following checks are reproducible without Qt:

```bash
cmake --preset selftest-only
cmake --build --preset selftest-only --parallel
ctest --preset selftest-only --output-on-failure
```

Expected tests:

```text
raw-entry-hotpatch-wobble
raw-entry-hotpatch-reverse
python-tool-tests
```

The same suite can be built with GCC interprocedural optimization enabled:

```bash
cmake --preset selftest-lto
cmake --build --preset selftest-lto --parallel
ctest --preset selftest-lto --output-on-failure
```

The CET/IBT layout has its own reproducible build:

```bash
cmake --preset selftest-cet
cmake --build --preset selftest-cet --parallel
ctest --preset selftest-cet --output-on-failure
```

Its target disassembly begins with `endbr64`, followed by 16 NOP bytes. The
patcher leaves `endbr64` intact and changes the bytes beginning at `entry + 4`.
The tests also reject a CET-prepared target paired with a replacement lacking
an `ENDBR64` landing pad.

The Release compile database should show `-O3` for `src/cube_step.cpp`.

Inspect the prepared entry:

```bash
objdump -d -M intel build/selftest/hotpatch_selftest \
  | sed -n '/<cube_step_builtin_v1>:/,+20p'
readelf -SW build/selftest/hotpatch_selftest \
  | grep __patchable_function_entries
```

Build a fresh patch module using the build oracle rather than CMake's patch
target, then exercise redirect and rollback:

```bash
python3 tools/compile_snippet.py \
  --compile-db build/selftest/compile_commands.json \
  --context src/cube_step.cpp \
  --source patches/wobble_patch.cpp \
  --output build/selftest/oracle_wobble.so \
  --optimization O3 --respect-access-control

./build/selftest/hotpatch_selftest build/selftest/oracle_wobble.so
```

The one-command patch compiler/activator has a compile-only validation mode:

```bash
python3 tools/jit_patch.py patches/reverse_patch.cpp \
  --compile-db build/selftest/compile_commands.json \
  --context src/cube_step.cpp \
  --output build/selftest/jit_reverse.so \
  --compile-only --respect-access-control

./build/selftest/hotpatch_selftest build/selftest/jit_reverse.so
```

The GUI validation command, once Qt 6 is present, is:

```bash
cmake --preset release
cmake --build --preset release --parallel
python3 tools/smoke_test.py --build-dir build/release --keep-runtime
```
