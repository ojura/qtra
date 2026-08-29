#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
with_gui=0
if [[ ${1:-} == "--with-gui" ]]; then
  with_gui=1
elif [[ $# -ne 0 ]]; then
  echo "usage: $0 [--with-gui]" >&2
  exit 2
fi

for preset in release release-lto release-cet; do
  "$root/scripts/build.sh" "$preset"
done

validation_dir="$root/build/oracle-validation"
rm -rf "$validation_dir"
mkdir -p "$validation_dir"
for build_name in release release-lto release-cet; do
  output="$validation_dir/oracle-$build_name.so"
  python3 "$root/tools/compile_snippet.py" \
    --compile-db "$root/build/$build_name/compile_commands.json" \
    --context "hotpatch_selftest.dir/src/cube_step.cpp.o" \
    --source "$root/patches/wobble_patch.cpp" \
    --output "$output" \
    --optimization O3 \
    --respect-access-control \
    > "$validation_dir/oracle-$build_name.command.json"
  "$root/build/$build_name/hotpatch_selftest" "$output"
done

python3 -m py_compile "$root"/tools/*.py
bash -n "$root"/scripts/*.sh

if (( with_gui )); then
  python3 "$root/tools/smoke_test.py" \
    --build-dir "$root/build/release" --keep-runtime
fi
