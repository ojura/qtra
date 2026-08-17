#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
preset=${1:-release}

cmake --preset "$preset" -S "$root"
cmake --build --preset "$preset" --parallel

if [[ "$preset" == selftest-* || "$preset" == "selftest-only" ]]; then
  ctest --preset "$preset"
fi
