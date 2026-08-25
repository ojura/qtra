#!/usr/bin/env python3
"""CLI client for the Qt runtime-agent JSON-lines Unix socket."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import select
import shutil
import socket
import sys
import time
from collections import deque
from pathlib import Path
from typing import Any, Callable, Deque, Mapping

# The executor a reload runs the new generation under when the caller does not
# say. It was spelled in three places, which is three places to disagree.
EXECUTORS = ("gui", "object", "render")
DEFAULT_RELOAD_EXECUTOR = "render"

JsonObject = dict[str, Any]
EventHandler = Callable[[JsonObject], None]


class ProtocolError(RuntimeError):
    """The agent returned an error response or malformed protocol data.

    The agent's error code is kept as an attribute, not just folded into the
    message, because callers have to act differently on some of them: a module
    declaring no release is an answer to work with, while a release that ran and
    failed is a reason to stop.
    """

    def __init__(self, message: str, code: str | None = None) -> None:
        super().__init__(message)
        self.code = code


class AgentClient:
    def __init__(self, socket_path: str, timeout: float = 10.0) -> None:
        self.socket_path = socket_path
        self.timeout = timeout
        self._socket: socket.socket | None = None
        self._buffer = bytearray()
        self._messages: Deque[JsonObject] = deque()
        # Events seen while waiting for a command response. Without this they
        # were dropped, so an operation.finished arriving during an unrelated
        # request left a later wait_for_operation waiting for something that had
        # already happened, which reads as the application hanging. The bound is
        # this client's own choice and is not tied to the agent's retained-event
        # count; dropping oldest first fails safe, because a wait for a dropped
        # completion times out rather than matching the wrong one.
        self._deferred_events: Deque[JsonObject] = deque(maxlen=1024)
        self._next_id = 1

    def __enter__(self) -> "AgentClient":
        self.connect()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    def connect(self) -> None:
        if self._socket is not None:
            return
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.setblocking(False)
        try:
            sock.connect(self.socket_path)
        except BlockingIOError:
            pass
        deadline = time.monotonic() + self.timeout
        while True:
            _, writable, exceptional = select.select([], [sock], [sock], self._remaining(deadline))
            if exceptional:
                sock.close()
                raise ConnectionError(f"could not connect to {self.socket_path}")
            if writable:
                error = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                if error:
                    sock.close()
                    raise OSError(error, os.strerror(error), self.socket_path)
                break
        self._socket = sock

    def close(self) -> None:
        if self._socket is not None:
            self._socket.close()
            self._socket = None

    def request(
        self,
        command: str,
        params: Mapping[str, Any] | None = None,
        *,
        timeout: float | None = None,
        on_event: EventHandler | None = None,
    ) -> Any:
        request_id = self._next_id
        self._next_id += 1
        self.send({"id": request_id, "command": command, "params": dict(params or {})})
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)

        while True:
            message = self.receive(deadline=deadline)
            if "event" in message:
                if on_event is not None:
                    on_event(message)
                self._deferred_events.append(message)
                continue
            if message.get("id") != request_id:
                raise ProtocolError(
                    f"unexpected response id {message.get('id')!r}; expected {request_id!r}"
                )
            if not message.get("ok", False):
                error = message.get("error") or {}
                code = error.get("code", "agent_error")
                text = error.get("message", "agent command failed")
                raise ProtocolError(f"{code}: {text}", code)
            return message.get("result")

    def subscribe(
        self,
        prefixes: list[str] | None = None,
        *,
        all_events: bool = False,
        on_event: EventHandler | None = None,
    ) -> Any:
        return self.request(
            "event.subscribe",
            {"all": all_events, "prefixes": prefixes or []},
            on_event=on_event,
        )

    def wait_for_operation(
        self,
        operation_id: str | int,
        *,
        timeout: float | None = None,
        on_event: EventHandler | None = None,
    ) -> JsonObject:
        wanted = str(operation_id)
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)

        # Anything already seen while awaiting an earlier response. These are not
        # passed to on_event: a caller that had a handler in that request has
        # seen them once already, and one that did not has no handler to miss.
        for index, message in enumerate(self._deferred_events):
            if message.get("event") != "operation.finished":
                continue
            data = message.get("data") or {}
            if str(data.get("operationId")) == wanted:
                del self._deferred_events[index]
                return message

        while True:
            message = self.receive(deadline=deadline)
            if "event" not in message:
                continue
            if on_event is not None:
                on_event(message)
            if message.get("event") != "operation.finished":
                continue
            data = message.get("data") or {}
            if str(data.get("operationId")) == wanted:
                return message
            # Another operation's completion, read off the socket while waiting
            # for this one. Retained for the same reason request() retains: a
            # later wait for it would otherwise sit until its timeout for
            # something that has already happened.
            self._deferred_events.append(message)

    def send(self, message: Mapping[str, Any]) -> None:
        if self._socket is None:
            self.connect()
        assert self._socket is not None
        payload = json.dumps(message, separators=(",", ":"), ensure_ascii=False).encode("utf-8") + b"\n"
        view = memoryview(payload)
        deadline = time.monotonic() + self.timeout
        while view:
            _, writable, _ = select.select([], [self._socket], [], self._remaining(deadline))
            if not writable:
                raise TimeoutError("timed out writing to runtime agent")
            sent = self._socket.send(view)
            if sent == 0:
                raise ConnectionError("runtime agent closed the socket while writing")
            view = view[sent:]

    def receive(self, *, deadline: float | None = None) -> JsonObject:
        if self._messages:
            return self._messages.popleft()
        if self._socket is None:
            self.connect()
        assert self._socket is not None
        if deadline is None:
            deadline = time.monotonic() + self.timeout

        while True:
            self._extract_messages()
            if self._messages:
                return self._messages.popleft()
            readable, _, _ = select.select([self._socket], [], [], self._remaining(deadline))
            if not readable:
                raise TimeoutError("timed out waiting for runtime-agent data")
            chunk = self._socket.recv(64 * 1024)
            if not chunk:
                raise ConnectionError("runtime agent closed the socket")
            self._buffer.extend(chunk)
            if len(self._buffer) > 2 * 1024 * 1024:
                raise ProtocolError("runtime-agent response buffer exceeded 2 MiB")

    def _extract_messages(self) -> None:
        while True:
            newline = self._buffer.find(b"\n")
            if newline < 0:
                return
            raw = bytes(self._buffer[:newline]).strip()
            del self._buffer[: newline + 1]
            if not raw:
                continue
            try:
                value = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise ProtocolError(f"invalid JSON from agent: {exc}: {raw[:200]!r}") from exc
            if not isinstance(value, dict):
                raise ProtocolError(f"agent message is not an object: {value!r}")
            self._messages.append(value)

    @staticmethod
    def _remaining(deadline: float) -> float:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("runtime-agent operation timed out")
        return remaining


def default_socket_path() -> str:
    return os.environ.get(
        "QT_RUNTIME_AGENT_SOCKET",
        f"/tmp/qt-runtime-cube-{os.getuid()}.sock",
    )


def parse_json(text: str | None, path: str | None = None) -> Any:
    if text is not None and path is not None:
        raise ValueError("use either inline JSON or a JSON file, not both")
    if path is not None:
        text = Path(path).read_text(encoding="utf-8")
    if text is None:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid JSON: {exc}") from exc


def print_json(value: Any, compact: bool = False, stream: Any | None = None) -> None:
    if stream is None:
        stream = sys.stdout
    if compact:
        print(json.dumps(value, separators=(",", ":"), ensure_ascii=False), file=stream)
    else:
        print(json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False), file=stream)


def fresh_object_path(source: Path) -> Path:
    """A path for `source`'s bytes that no dlopen() in the process has seen.

    dlopen() keys loaded objects by pathname, so asking for one that is already
    loaded returns the resident handle and the old code with it, whatever the
    file on disk now contains. Copying under a name nothing has requested is
    what makes a rebuilt object actually take effect.
    """
    digest = hashlib.sha256(source.read_bytes() + str(time.time_ns()).encode("ascii"))
    directory = source.parent / "runtime-snippets"
    directory.mkdir(parents=True, exist_ok=True)
    return directory / f"{source.stem}-{digest.hexdigest()[:12]}{source.suffix}"


def loaded_snippet_modules(client: "AgentClient") -> list[JsonObject]:
    modules = client.request("module.list")
    if not isinstance(modules, list):
        return []
    return [m for m in modules if isinstance(m, dict) and m.get("kind") == "snippet"]


def previous_generation(modules: list[JsonObject], name: str, exclude_id: str) -> JsonObject | None:
    """The newest other module reporting the same snippet name.

    Generations of one snippet differ in path, since each reload needs a new one,
    but the descriptor name is the same string in every build of that source,
    which makes it the reliable way to recognise them as the same thing.
    """
    candidates = [
        m for m in modules
        if m.get("name") == name and str(m.get("id")) != str(exclude_id)
    ]
    if not candidates:
        return None
    return max(candidates, key=lambda m: int(m["id"]))


def run_snippet(
    client: "AgentClient",
    module_id: str,
    executor: str,
    request: Any,
    target: JsonObject | None,
    timeout: float,
    on_event: EventHandler | None = None,
) -> JsonObject:
    params: JsonObject = {"moduleId": module_id, "executor": executor, "request": request}
    if target:
        params["target"] = target
    started = client.request("snippet.run", params, on_event=on_event)
    finished = client.wait_for_operation(
        started["operationId"], timeout=timeout, on_event=on_event
    )
    return {"started": started, "finished": finished}


def release_snippet(
    client: "AgentClient",
    module_id: str,
    executor: str | None,
    target: JsonObject | None,
    timeout: float,
    on_event: EventHandler | None = None,
) -> tuple[str, JsonObject]:
    """Ask a module to undo what it installed, through its declared entry point.

    Returns the outcome as one of "released", "declared-none" or "failed", so a
    caller can act on the difference. Sending a payload and reading "completed"
    off it cannot make that distinction: a module that ignores the payload
    reports success having done nothing.

    Executor and target are left out unless the caller insists on them, because
    the agent knows how the module was last run successfully and that is where
    its release belongs.
    """
    params: JsonObject = {"moduleId": module_id}
    if executor:
        params["executor"] = executor
    if target:
        params["target"] = target
    # Only a release that ran and went wrong is a failure. A module that
    # declares none, or that has never run and so cannot have installed
    # anything, is answering the question, and treating either as a failure
    # would stop a handover that had nothing to do in the first place.
    not_a_failure = {
        "no_release_declared": "declared-none",
        # The agent now answers this only when the module has never been run
        # at all, so it cannot have installed anything and there is nothing
        # for a handover to undo. A run that installed and then failed
        # resolves to the executor that attempt used.
        "no_recorded_executor": "never-ran",
    }
    try:
        started = client.request("snippet.release", params, on_event=on_event)
    except ProtocolError as exc:
        return not_a_failure.get(exc.code or "", "failed"), {"reason": str(exc)}

    finished = client.wait_for_operation(
        started["operationId"], timeout=timeout, on_event=on_event
    )
    data = finished.get("data") or {}
    detail: JsonObject = {"started": started, "finished": finished}
    if data.get("outcome") != "completed":
        return "failed", detail
    return "released", detail


def hand_over(
    client: "AgentClient",
    outgoing: JsonObject,
    executor: str | None,
    target: JsonObject | None,
    handover_request: str | None,
    timeout: float,
    on_event: EventHandler | None = None,
) -> JsonObject:
    """Get the outgoing generation to let go before the new one installs.

    The declared release is tried first because it is the only route that can
    report whether anything happened. A module declaring none falls back to a
    request payload if the caller supplied one, which is an application's own
    convention rather than a property of the mechanism, and is reported under a
    different outcome so the two are never confused.
    """
    outcome, detail = release_snippet(
        client, outgoing["id"], executor, target, timeout, on_event
    )

    if outcome != "declared-none" or handover_request is None:
        return {"moduleId": outgoing["id"], "route": "release", "outcome": outcome, **detail}

    ran = run_snippet(
        client,
        outgoing["id"],
        executor or DEFAULT_RELOAD_EXECUTOR,
        parse_json(handover_request),
        target,
        timeout,
        on_event,
    )
    data = (ran.get("finished") or {}).get("data") or {}
    return {
        "moduleId": outgoing["id"],
        "route": "handover-request",
        "outcome": "payload-sent" if data.get("outcome") == "completed" else "failed",
        "note": "this module declares no release; the payload's effect is unverifiable",
        **ran,
    }


def is_finished_operation(message: Mapping[str, Any], operation_id: str | int) -> bool:
    if message.get("event") != "operation.finished":
        return False
    data = message.get("data") or {}
    return isinstance(data, Mapping) and str(data.get("operationId")) == str(operation_id)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default=default_socket_path(), help="agent Unix socket path")
    parser.add_argument("--timeout", type=float, default=10.0, help="timeout in seconds")
    parser.add_argument("--compact", action="store_true", help="emit compact JSON")
    parser.add_argument(
        "--show-events", action="store_true", help="print events seen while awaiting responses"
    )
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    call = subparsers.add_parser("call", help="invoke one protocol command")
    call.add_argument("command")
    call.add_argument("--params", help="inline JSON object")
    call.add_argument("--params-file", help="path to JSON object")

    events = subparsers.add_parser("events", help="subscribe and stream events")
    events.add_argument("--prefix", action="append", default=[], help="event-name prefix")
    events.add_argument("--all", action="store_true", help="subscribe to all events")
    events.add_argument("--count", type=int, default=0, help="stop after N events (0 = unlimited)")
    events.add_argument("--seconds", type=float, default=0.0, help="stop after this duration")

    history = subparsers.add_parser("history", help="read retained events without subscribing")
    history.add_argument("--after", default="0", help="return sequences greater than this value")
    history.add_argument("--limit", type=int, default=256, help="maximum retained events")
    history.add_argument("--prefix", action="append", default=[], help="event-name prefix")

    wait = subparsers.add_parser("wait", help="wait for operation.finished")
    wait.add_argument("operation_id")

    patch = subparsers.add_parser("patch", help="load and activate a cube patch module")
    patch.add_argument("path")
    patch.add_argument("--mode", choices=("dispatch", "entry"), default="dispatch")

    snippet = subparsers.add_parser("snippet", help="load, run, and optionally await a snippet")
    snippet.add_argument("path")
    snippet.add_argument("--executor", choices=EXECUTORS, default="gui")
    target = snippet.add_mutually_exclusive_group()
    target.add_argument("--target-name")
    target.add_argument("--target-id")
    snippet.add_argument("--request", help="inline JSON request")
    snippet.add_argument("--request-file", help="path to JSON request")
    snippet.add_argument("--no-wait", action="store_true")

    reload_cmd = subparsers.add_parser(
        "reload",
        help="replace a loaded snippet with a rebuilt object of the same name",
        description="Take the installed generation off, then load the rebuilt object "
                    "under a path dlopen() has not seen, so the new code takes effect "
                    "in the running process.",
    )
    reload_cmd.add_argument("path", help="the rebuilt shared object")
    # No default, so that leaving it out can mean something. The new generation
    # runs under render unless told otherwise, but the handover is left to the
    # executor the agent recorded for the outgoing module, which is where its
    # install actually ran. Naming one here overrides both.
    reload_cmd.add_argument("--executor", choices=EXECUTORS, default=None)
    reload_target = reload_cmd.add_mutually_exclusive_group()
    reload_target.add_argument("--target-name")
    reload_target.add_argument("--target-id")
    reload_cmd.add_argument("--request", help="inline JSON request for the new generation")
    reload_cmd.add_argument("--request-file", help="path to JSON request")
    reload_cmd.add_argument(
        "--replace", help="module id to hand over from, instead of matching by snippet name"
    )
    # Only for modules that declare no release. What such a module has to be
    # told in order to let go is its own business; nothing in the ABI or the
    # protocol defines it, and the scene snippets here happen to use
    # {"restore": true}. A module that declares a release needs none of this.
    reload_cmd.add_argument(
        "--handover-request",
        help="fallback request for an outgoing generation that declares no release; "
             "its effect cannot be verified",
    )
    reload_cmd.add_argument(
        "--force",
        action="store_true",
        help="run the new generation even if the outgoing one failed to release",
    )

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    def show_event(message: JsonObject) -> None:
        if args.show_events:
            print_json(message, args.compact, stream=sys.stderr)

    try:
        with AgentClient(args.socket, args.timeout) as client:
            if args.subcommand == "call":
                params = parse_json(args.params, args.params_file)
                if not isinstance(params, dict):
                    raise ValueError("command params must be a JSON object")
                print_json(
                    client.request(args.command, params, on_event=show_event), args.compact
                )
                return 0

            if args.subcommand == "events":
                client.subscribe(args.prefix, all_events=args.all, on_event=show_event)
                started = time.monotonic()
                stop_at = started + args.seconds if args.seconds > 0 else None
                seen = 0
                while True:
                    if stop_at is not None and time.monotonic() >= stop_at:
                        return 0
                    deadline = (
                        min(stop_at, time.monotonic() + args.timeout)
                        if stop_at is not None
                        else time.monotonic() + args.timeout
                    )
                    try:
                        message = client.receive(deadline=deadline)
                    except TimeoutError:
                        # Event streaming may legitimately be quiet. A finite
                        # --seconds window ends successfully; an unbounded stream
                        # simply starts another receive interval.
                        if stop_at is not None and time.monotonic() >= stop_at:
                            return 0
                        continue
                    if "event" not in message:
                        continue
                    print_json(message, args.compact)
                    seen += 1
                    if args.count > 0 and seen >= args.count:
                        return 0

            if args.subcommand == "history":
                print_json(
                    client.request(
                        "event.history",
                        {
                            "afterSequence": args.after,
                            "limit": args.limit,
                            "prefixes": args.prefix,
                        },
                        on_event=show_event,
                    ),
                    args.compact,
                )
                return 0

            if args.subcommand == "wait":
                terminal_events: list[JsonObject] = []

                def capture_operation(message: JsonObject) -> None:
                    show_event(message)
                    if is_finished_operation(message, args.operation_id):
                        terminal_events.append(message)

                client.subscribe(["operation."], on_event=capture_operation)
                history = client.request(
                    "event.history",
                    {"prefixes": ["operation.finished"], "limit": 1024},
                    on_event=capture_operation,
                )
                for message in history:
                    if isinstance(message, dict) and is_finished_operation(
                        message, args.operation_id
                    ):
                        terminal_events.append(message)
                result = terminal_events[-1] if terminal_events else client.wait_for_operation(
                    args.operation_id,
                    timeout=args.timeout,
                    on_event=capture_operation,
                )
                print_json(result, args.compact)
                return 0

            if args.subcommand == "patch":
                loaded = client.request(
                    "patch.load", {"path": str(Path(args.path).resolve())}, on_event=show_event
                )
                activated = client.request(
                    "patch.activate",
                    {"moduleId": loaded["moduleId"], "mode": args.mode},
                    on_event=show_event,
                )
                print_json({"loaded": loaded, "activated": activated}, args.compact)
                return 0

            if args.subcommand == "snippet":
                request = parse_json(args.request, args.request_file)
                resolved = str(Path(args.path).resolve())
                # Loading a path that is already loaded succeeds and returns a
                # fresh module id, but dlopen() hands back the resident object,
                # so a rebuilt file at that path runs its old code with nothing
                # reporting a problem. Say so rather than let it look like a
                # rebuild that did not take.
                if any(m.get("path") == resolved for m in loaded_snippet_modules(client)):
                    print(
                        f"agentctl: {resolved} is already loaded; dlopen() will return the "
                        f"resident copy and any rebuild of this file will not take effect. "
                        f"Use 'agentctl.py reload' to load it as a new generation.",
                        file=sys.stderr,
                    )
                loaded = client.request(
                    "snippet.load", {"path": resolved}, on_event=show_event
                )
                params: JsonObject = {
                    "moduleId": loaded["moduleId"],
                    "executor": args.executor,
                    "request": request,
                }
                if args.executor == "object":
                    if args.target_name:
                        params["target"] = {"objectName": args.target_name}
                    elif args.target_id:
                        params["target"] = {"id": args.target_id}
                    else:
                        raise ValueError("object executor requires --target-name or --target-id")
                client.subscribe(["operation."], on_event=show_event)
                started = client.request("snippet.run", params, on_event=show_event)
                result: JsonObject = {"loaded": loaded, "started": started}
                if not args.no_wait:
                    result["finished"] = client.wait_for_operation(
                        started["operationId"], timeout=args.timeout, on_event=show_event
                    )
                print_json(result, args.compact)
                return 0

            if args.subcommand == "reload":
                source = Path(args.path).resolve()
                if not source.is_file():
                    raise ValueError(f"not a regular file: {source}")
                request = parse_json(args.request, args.request_file)
                target: JsonObject | None = None
                if args.executor == "object":
                    if args.target_name:
                        target = {"objectName": args.target_name}
                    elif args.target_id:
                        target = {"id": args.target_id}
                    else:
                        raise ValueError("object executor requires --target-name or --target-id")

                client.subscribe(["operation."], on_event=show_event)
                run_executor = args.executor or DEFAULT_RELOAD_EXECUTOR
                result: JsonObject = {}

                # With an explicit id the outgoing module is known before
                # anything is loaded, so the handover runs first and a failure
                # costs nothing at all: no copy, no load, no new module left
                # resident. Without one, generations are recognised by descriptor
                # name, and the name only exists once the new object is loaded.
                outgoing: JsonObject | None = None
                if args.replace:
                    outgoing = next(
                        (m for m in loaded_snippet_modules(client)
                         if str(m.get("id")) == str(args.replace)),
                        None,
                    )
                    if outgoing is None:
                        raise ValueError(f"no loaded snippet module with id {args.replace}")
                    result["handover"] = hand_over(
                        client, outgoing, args.executor, target,
                        args.handover_request, args.timeout, show_event,
                    )
                    if result["handover"]["outcome"] == "failed" and not args.force:
                        result["installed"] = False
                        result["note"] = ("the outgoing generation did not release, so nothing "
                                          "was loaded; pass --force to install anyway")
                        print_json(result, args.compact)
                        return 1

                fresh = fresh_object_path(source)
                shutil.copy2(source, fresh)
                loaded = client.request(
                    "snippet.load", {"path": str(fresh)}, on_event=show_event
                )
                result["copiedTo"] = str(fresh)
                result["loaded"] = loaded

                if outgoing is None:
                    outgoing = previous_generation(
                        loaded_snippet_modules(client), loaded.get("name", ""), loaded["moduleId"]
                    )
                    if outgoing is not None:
                        result["handover"] = hand_over(
                            client, outgoing, args.executor, target,
                            args.handover_request, args.timeout, show_event,
                        )
                result["supersedes"] = outgoing["id"] if outgoing else None

                # A module is never unloaded, so a failed release cannot be
                # undone by dropping the new object. What is still avoidable is
                # running its install on top of state the old one still holds,
                # which is the failure this whole path exists to prevent. The
                # new module stays resident and inert, which costs almost
                # nothing.
                handover = result.get("handover") or {}
                if handover.get("outcome") == "failed" and not args.force:
                    result["installed"] = False
                    result["note"] = ("the outgoing generation did not release, so the new one "
                                      "was loaded but not run; pass --force to run it anyway")
                    print_json(result, args.compact)
                    return 1

                ran = run_snippet(
                    client,
                    loaded["moduleId"],
                    run_executor,
                    request,
                    target,
                    args.timeout,
                    show_event,
                )
                result.update(ran)

                # The handover path exits non-zero when it goes wrong, and the
                # install has to do the same. Otherwise a caller chaining on
                # success carries on from a generation that refused to install,
                # having been told the reload worked.
                outcome = ((ran.get("finished") or {}).get("data") or {}).get("outcome")
                result["installed"] = outcome == "completed"
                if not result["installed"]:
                    result["note"] = ("the new generation was loaded but its run did not "
                                      "complete, so it may have installed nothing")
                    print_json(result, args.compact)
                    return 1
                print_json(result, args.compact)
                return 0

    except (ConnectionError, OSError, ProtocolError, TimeoutError, ValueError) as exc:
        print(f"agentctl: {exc}", file=sys.stderr)
        return 2

    parser.error("unhandled subcommand")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
