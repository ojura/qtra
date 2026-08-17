#!/usr/bin/env python3
"""End-to-end GUI smoke test for a built runtime-agent demo."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from agentctl import AgentClient  # noqa: E402


def wait_for_socket(path: Path, process: subprocess.Popen[Any], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"application exited early with status {process.returncode}")
        if path.exists():
            try:
                with AgentClient(os.fspath(path), timeout=0.5) as client:
                    client.request("hello", timeout=0.5)
                return
            except (ConnectionError, OSError, TimeoutError):
                pass
        time.sleep(0.05)
    raise TimeoutError(f"runtime-agent socket did not appear: {path}")


def module_id(result: dict[str, Any]) -> str:
    value = result.get("moduleId")
    if value is None:
        raise RuntimeError(f"module load response lacks moduleId: {result}")
    return str(value)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent.parent
    parser.add_argument("--build-dir", type=Path, default=root / "build" / "release")
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--keep-runtime", action="store_true")
    args = parser.parse_args(argv)

    build_dir = args.build_dir.resolve()
    app = build_dir / "qt_runtime_cube"
    required = {
        "app": app,
        "patch": build_dir / "cube_patch_wobble.so",
        "inspect": build_dir / "agent_snippet_inspect.so",
        "observer": build_dir / "agent_snippet_observer.so",
        "render": build_dir / "agent_snippet_render_probe.so",
    }
    missing = [str(path) for path in required.values() if not path.is_file()]
    if missing:
        print("missing built artifacts:\n  " + "\n  ".join(missing), file=sys.stderr)
        return 2

    runtime = Path(tempfile.mkdtemp(prefix="qt-runtime-agent-smoke-"))
    socket_path = runtime / "agent.sock"
    capture_path = runtime / "cube.png"
    log_path = runtime / "app.log"

    command: list[str]
    if os.environ.get("DISPLAY"):
        command = [str(app)]
    elif shutil.which("xvfb-run"):
        command = [
            "xvfb-run",
            "-a",
            "-s",
            "-screen 0 1280x800x24 +extension GLX +render -noreset",
            str(app),
        ]
    else:
        print("DISPLAY is unset and xvfb-run is unavailable", file=sys.stderr)
        return 2
    command += ["--agent-socket", str(socket_path)]

    environment = os.environ.copy()
    environment.setdefault("LIBGL_ALWAYS_SOFTWARE", "1")
    log = log_path.open("wb")
    process = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT)

    transcript: dict[str, Any] = {}
    try:
        wait_for_socket(socket_path, process, args.timeout)
        with AgentClient(str(socket_path), timeout=args.timeout) as client:
            client.subscribe(all_events=True)
            transcript["hello"] = client.request("hello")
            transcript["initialState"] = client.request("cube.state")
            transcript["tree"] = client.request("object.tree", {"maxDepth": 2})

            loaded_patch = client.request("patch.load", {"path": str(required["patch"])})
            transcript["dispatchPatch"] = client.request(
                "patch.activate", {"moduleId": module_id(loaded_patch), "mode": "dispatch"}
            )
            transcript["dispatchRollback"] = client.request("patch.rollback")
            transcript["entryPatch"] = client.request(
                "patch.activate", {"moduleId": module_id(loaded_patch), "mode": "entry"}
            )
            transcript["entryRollback"] = client.request("patch.rollback")

            loaded_inspect = client.request(
                "snippet.load", {"path": str(required["inspect"])}
            )
            started = client.request(
                "snippet.run",
                {
                    "moduleId": module_id(loaded_inspect),
                    "executor": "gui",
                    "request": {"nudgeAngle": 7.5, "setSpeed": 110.0},
                },
            )
            transcript["inspectSnippet"] = client.wait_for_operation(
                started["operationId"], timeout=args.timeout
            )

            loaded_observer = client.request(
                "snippet.load", {"path": str(required["observer"])}
            )
            started = client.request(
                "snippet.run",
                {
                    "moduleId": module_id(loaded_observer),
                    "executor": "gui",
                    "request": {},
                },
            )
            transcript["observerSnippet"] = client.wait_for_operation(
                started["operationId"], timeout=args.timeout
            )

            loaded_render = client.request(
                "snippet.load", {"path": str(required["render"])}
            )
            started = client.request(
                "snippet.run",
                {
                    "moduleId": module_id(loaded_render),
                    "executor": "render",
                    "request": {},
                },
            )
            transcript["renderSnippet"] = client.wait_for_operation(
                started["operationId"], timeout=args.timeout
            )

            transcript["capture"] = client.request(
                "cube.capture", {"path": str(capture_path)}
            )
            if capture_path.read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
                raise RuntimeError("framebuffer capture is not a PNG")
            transcript["finalState"] = client.request("cube.state")
            transcript["eventHistory"] = client.request(
                "event.history",
                {"prefixes": ["operation.finished"], "limit": 32},
            )
            client.request("process.quit")

        process.wait(timeout=args.timeout)
        transcript["processExit"] = process.returncode
        transcript["runtimeDirectory"] = str(runtime)
        print(json.dumps(transcript, indent=2, sort_keys=True))
        return 0 if process.returncode == 0 else 1
    except BaseException as exc:
        print(f"smoke test failed: {exc}", file=sys.stderr)
        return 1
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        log.close()
        if not args.keep_runtime and process.returncode == 0:
            shutil.rmtree(runtime, ignore_errors=True)
        else:
            print(f"runtime artifacts: {runtime}", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
