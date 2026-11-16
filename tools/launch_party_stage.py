#!/usr/bin/env python3
"""Start or reuse the native LARICS party display on the current desktop."""

from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import time

from agentctl import AgentClient

ROOT = Path(__file__).resolve().parents[1]
NAMES = {
    "audio_pulse": "drive the cube from the system audio mix",
    "orbit_sphere": "draw a sphere orbiting the cube",
    "mobius_ring": "rotating translucent Mobius strip around the scene",
    "party_stage": "LARICS / LYRICS party stage",
}


def connect(socket: Path) -> AgentClient | None:
    if socket.exists():
        info = socket.stat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
            raise RuntimeError(f"The requested socket is not a socket owned by this user: {socket}")
    client = AgentClient(str(socket), timeout=20)
    try:
        client.connect()
    except (FileNotFoundError, ConnectionRefusedError):
        client.close()
        return None
    return client


def matching_process(binary: Path, socket: Path) -> int | None:
    """Find only a same-user qtra with this exact binary and socket argument."""
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.stat().st_uid != os.getuid():
                continue
            args = (entry / "cmdline").read_bytes().split(b"\0")
            if not args or os.fsdecode(args[0]) != str(binary):
                continue
            index = args.index(b"--agent-socket")
            if os.fsdecode(args[index + 1]) == str(socket):
                return int(entry.name)
        except (OSError, ValueError, IndexError):
            continue
    return None


def invoke(client: AgentClient, module: str, executor: str, request: dict) -> dict:
    operation = client.request("snippet.run", {"moduleId": module, "executor": executor, "request": request})
    result = client.wait_for_operation(operation["operationId"], timeout=25)["data"]
    if result["outcome"] != "completed":
        raise RuntimeError(result.get("error", str(result)))
    return result["result"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "release")
    parser.add_argument("--state-file", type=Path, default=Path.home() / ".local/state/larics-dj/state.json")
    parser.add_argument("--runtime-dir", type=Path, help="PID, module IDs and log folder; defaults to the state file's folder")
    parser.add_argument("--socket", type=Path)
    parser.add_argument("--base-url", help="Native request/chat API base, such as http://127.0.0.1:8787")
    parser.add_argument("--fullscreen", action="store_true")
    parser.add_argument("--panel", action="store_true")
    options = parser.parse_args()
    build = options.build_dir.expanduser().resolve()
    state_file = options.state_file.expanduser().resolve()
    runtime = (options.runtime_dir or state_file.parent).expanduser().resolve()
    runtime.mkdir(parents=True, exist_ok=True)
    user_runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
    socket = (options.socket or (user_runtime if user_runtime.is_dir() else runtime) / "larics-dj-qtra.sock").expanduser().absolute()
    binary = build / "qt_runtime_cube"
    required = [binary, *(build / f"agent_snippet_{name}.so" for name in NAMES)]
    absent = [str(p) for p in required if not p.is_file()]
    if absent:
        raise RuntimeError("Build the party modules first; missing: " + ", ".join(absent))
    metadata_file = runtime / "qtra-modules.json"
    process = None
    client = None
    socket.parent.mkdir(parents=True, exist_ok=True)
    lock = os.fdopen(os.open(str(socket) + ".launch.lock", os.O_CREAT | os.O_RDWR, 0o600), "a")
    try:
        # Startup, module loading and the metadata replacement all belong to
        # one invocation when two launcher commands arrive together.
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        previous = {}
        try:
            previous = json.loads(metadata_file.read_text())
        except (OSError, ValueError):
            pass
        if not isinstance(previous, dict):
            previous = {}
        client = connect(socket)
        if client is None:
            existing_pid = matching_process(binary, socket)
            if existing_pid is None:
                with (runtime / "qtra.log").open("ab") as log:
                    process = subprocess.Popen([str(binary), "--agent-socket", str(socket)], cwd=ROOT,
                                               stdout=log, stderr=log, start_new_session=True)
                (runtime / "qtra.pid").write_text(f"{process.pid}\n")
            deadline = time.monotonic() + 20
            while client is None and time.monotonic() < deadline:
                if process is not None and process.poll() is not None:
                    raise RuntimeError(f"qtra exited; inspect {runtime / 'qtra.log'}")
                time.sleep(.1)
                client = connect(socket)
            if client is None:
                raise RuntimeError(f"qtra PID {existing_pid or process.pid} is running but its socket is unavailable: {socket}")
        hello = client.request("hello", {})
        if hello.get("name") != "qt-runtime-cube" or hello.get("uid") != os.getuid():
            raise RuntimeError("The socket does not identify this user's qtra host")
        loaded = client.request("module.list", {})
        by_id = {str(item["id"]): item for item in loaded}
        remembered = previous.get("modules", {}) if previous.get("pid") == hello["pid"] else {}
        if not isinstance(remembered, dict):
            remembered = {}
        modules = {}
        for name, descriptor in NAMES.items():
            module = str(remembered.get(name, ""))
            if module not in by_id or by_id[module].get("name") != descriptor:
                choices = [item for item in loaded if item.get("name") == descriptor]
                module = str(max(choices, key=lambda item: int(item["id"]))["id"]) if choices else ""
            newly_loaded = not module
            if newly_loaded:
                module = client.request("snippet.load", {"path": str(build / f"agent_snippet_{name}.so")})["moduleId"]
            modules[name] = module
            if name == "audio_pulse":
                invoke(client, module, "render", {"events": True})
            elif name in ("orbit_sphere", "mobius_ring") and newly_loaded:
                invoke(client, module, "render", {"restore": True})
        request = {"stateFile": str(state_file)}
        if options.fullscreen:
            request["fullscreen"] = True
        if options.panel:
            request["panel"] = True
        if options.base_url is not None:
            request["requestBaseUrl"] = options.base_url
        stage = invoke(client, modules["party_stage"], "gui", request)
        client.request("object.invoke", {"objectName": "mainWindow", "method": "show"})
        client.request("object.invoke", {"objectName": "mainWindow", "method": "raise"})
        metadata = {"pid": hello["pid"], "socket": str(socket), "modules": modules,
                    "stateFile": str(state_file), "buildDir": str(build)}
        if previous.get("pid") == hello["pid"] and previous.get("commit"):
            metadata["commit"] = previous["commit"]
        temporary = metadata_file.with_suffix(".new")
        temporary.write_text(json.dumps(metadata, indent=2) + "\n")
        temporary.replace(metadata_file)
        (runtime / "qtra.pid").write_text(f"{hello['pid']}\n")
        print(json.dumps({**metadata, "reused": process is None, "stage": stage}, ensure_ascii=False))
        return 0
    except BaseException:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(5)
        raise
    finally:
        if client is not None:
            client.close()
        lock.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Party stage: {error}", file=sys.stderr)
        raise SystemExit(1)
