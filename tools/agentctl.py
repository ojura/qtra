#!/usr/bin/env python3
"""CLI client for the Qt runtime-agent JSON-lines Unix socket."""

from __future__ import annotations

import argparse
import json
import os
import select
import socket
import sys
import time
from collections import deque
from pathlib import Path
from typing import Any, Callable, Deque, Mapping

JsonObject = dict[str, Any]
EventHandler = Callable[[JsonObject], None]


class ProtocolError(RuntimeError):
    """The agent returned an error response or malformed protocol data."""


class AgentClient:
    def __init__(self, socket_path: str, timeout: float = 10.0) -> None:
        self.socket_path = socket_path
        self.timeout = timeout
        self._socket: socket.socket | None = None
        self._buffer = bytearray()
        self._messages: Deque[JsonObject] = deque()
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
                continue
            if message.get("id") != request_id:
                raise ProtocolError(
                    f"unexpected response id {message.get('id')!r}; expected {request_id!r}"
                )
            if not message.get("ok", False):
                error = message.get("error") or {}
                code = error.get("code", "agent_error")
                text = error.get("message", "agent command failed")
                raise ProtocolError(f"{code}: {text}")
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
    snippet.add_argument("--executor", choices=("gui", "object", "render"), default="gui")
    target = snippet.add_mutually_exclusive_group()
    target.add_argument("--target-name")
    target.add_argument("--target-id")
    snippet.add_argument("--request", help="inline JSON request")
    snippet.add_argument("--request-file", help="path to JSON request")
    snippet.add_argument("--no-wait", action="store_true")

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
                loaded = client.request(
                    "snippet.load", {"path": str(Path(args.path).resolve())}, on_event=show_event
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

    except (ConnectionError, OSError, ProtocolError, TimeoutError, ValueError) as exc:
        print(f"agentctl: {exc}", file=sys.stderr)
        return 2

    parser.error("unhandled subcommand")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
