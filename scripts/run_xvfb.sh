#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${QT_RUNTIME_AGENT_BUILD_DIR:-"$root/build/release"}
app=${QT_RUNTIME_AGENT_APP:-"$build_dir/qt_runtime_cube"}

if [[ ! -x "$app" ]]; then
  echo "application not found or not executable: $app" >&2
  echo "build it with: $root/scripts/build.sh release" >&2
  exit 2
fi

export LIBGL_ALWAYS_SOFTWARE=${LIBGL_ALWAYS_SOFTWARE:-1}

if [[ -n ${DISPLAY:-} ]]; then
  exec "$app" "$@"
fi

if ! command -v xvfb-run >/dev/null 2>&1; then
  echo "DISPLAY is unset and xvfb-run is unavailable" >&2
  exit 2
fi

exec xvfb-run -a \
  -s "-screen 0 1280x800x24 +extension GLX +render -noreset" \
  "$app" "$@"
