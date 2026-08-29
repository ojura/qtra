#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${QT_RUNTIME_AGENT_BUILD_DIR:-"$root/build/release"}
ctl=(python3 "$root/tools/agentctl.py")

"${ctl[@]}" call hello
"${ctl[@]}" call object.tree --params '{"maxDepth":2}'
"${ctl[@]}" patch "$build/cube_patch_wobble.so"
"${ctl[@]}" call cube.state
"${ctl[@]}" call patch.rollback
"${ctl[@]}" snippet "$build/agent_snippet_inspect.so" \
  --request '{"nudgeAngle":15,"setSpeed":120}'
"${ctl[@]}" call cube.capture --params '{"path":"/tmp/qt-runtime-cube-demo.png"}'
