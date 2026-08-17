from __future__ import annotations

import json
import os
import socket
import tempfile
import threading
import time
import unittest
import sys
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from typing import Any

TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from agentctl import AgentClient, ProtocolError, main


class FakeAgent:
    def __init__(self, path: Path, handler: Any) -> None:
        self.path = path
        self.handler = handler
        self.error: BaseException | None = None
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self) -> "FakeAgent":
        self.thread.start()
        if not self.ready.wait(2):
            raise RuntimeError("fake agent did not start")
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.thread.join(2)
        if self.error is not None:
            raise self.error

    @staticmethod
    def send(connection: socket.socket, value: dict[str, Any]) -> None:
        connection.sendall(json.dumps(value).encode("utf-8") + b"\n")

    @staticmethod
    def receive(connection: socket.socket) -> dict[str, Any]:
        data = bytearray()
        while b"\n" not in data:
            chunk = connection.recv(4096)
            if not chunk:
                raise ConnectionError("client disconnected")
            data.extend(chunk)
        return json.loads(bytes(data.split(b"\n", 1)[0]))

    def _run(self) -> None:
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            server.bind(os.fspath(self.path))
            server.listen(1)
            self.ready.set()
            connection, _ = server.accept()
            with connection:
                self.send(connection, {"event": "agent.connected", "data": {}})
                self.handler(self, connection)
        except BaseException as exc:  # propagate test-server failures
            self.error = exc
            self.ready.set()
        finally:
            server.close()


class AgentClientTests(unittest.TestCase):
    def test_request_ignores_intermediate_event(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "agent.sock"

            def handler(agent: FakeAgent, connection: socket.socket) -> None:
                request = agent.receive(connection)
                agent.send(connection, {"event": "cube.frame", "data": {"frameIndex": "60"}})
                agent.send(
                    connection,
                    {"id": request["id"], "ok": True, "result": {"running": True}},
                )

            seen: list[str] = []
            with FakeAgent(path, handler):
                with AgentClient(os.fspath(path), timeout=2) as client:
                    result = client.request(
                        "cube.state", on_event=lambda event: seen.append(event["event"])
                    )
            self.assertEqual(result, {"running": True})
            self.assertEqual(seen, ["agent.connected", "cube.frame"])

    def test_agent_error_becomes_protocol_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "agent.sock"

            def handler(agent: FakeAgent, connection: socket.socket) -> None:
                request = agent.receive(connection)
                agent.send(
                    connection,
                    {
                        "id": request["id"],
                        "ok": False,
                        "error": {"code": "bad", "message": "broken"},
                    },
                )

            with FakeAgent(path, handler):
                with AgentClient(os.fspath(path), timeout=2) as client:
                    with self.assertRaisesRegex(ProtocolError, "bad: broken"):
                        client.request("bad.command")

    def test_wait_for_operation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "agent.sock"

            def handler(agent: FakeAgent, connection: socket.socket) -> None:
                agent.send(
                    connection,
                    {
                        "event": "operation.finished",
                        "data": {"operationId": "42", "outcome": "completed"},
                    },
                )

            with FakeAgent(path, handler):
                with AgentClient(os.fspath(path), timeout=2) as client:
                    result = client.wait_for_operation("42", timeout=2)
            self.assertEqual(result["data"]["outcome"], "completed")

    def test_history_subcommand(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "agent.sock"

            def handler(agent: FakeAgent, connection: socket.socket) -> None:
                request = agent.receive(connection)
                self.assertEqual(request["command"], "event.history")
                self.assertEqual(request["params"]["afterSequence"], "12")
                self.assertEqual(request["params"]["prefixes"], ["operation."])
                agent.send(
                    connection,
                    {
                        "id": request["id"],
                        "ok": True,
                        "result": [{"event": "operation.finished", "sequence": "13"}],
                    },
                )

            output = StringIO()
            with FakeAgent(path, handler), redirect_stdout(output):
                status = main(
                    [
                        "--socket",
                        os.fspath(path),
                        "history",
                        "--after",
                        "12",
                        "--prefix",
                        "operation.",
                    ]
                )
            self.assertEqual(status, 0)
            self.assertEqual(json.loads(output.getvalue())[0]["sequence"], "13")

    def test_quiet_timed_event_stream_finishes_successfully(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "agent.sock"

            def handler(agent: FakeAgent, connection: socket.socket) -> None:
                request = agent.receive(connection)
                self.assertEqual(request["command"], "event.subscribe")
                agent.send(connection, {"id": request["id"], "ok": True, "result": {}})
                time.sleep(0.2)

            output = StringIO()
            with FakeAgent(path, handler), redirect_stdout(output):
                status = main(
                    [
                        "--socket",
                        os.fspath(path),
                        "--timeout",
                        "0.03",
                        "events",
                        "--seconds",
                        "0.08",
                    ]
                )
            self.assertEqual(status, 0)
            self.assertEqual(output.getvalue(), "")

    def test_wait_subcommand_replays_finished_operation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "agent.sock"

            def handler(agent: FakeAgent, connection: socket.socket) -> None:
                subscribe = agent.receive(connection)
                self.assertEqual(subscribe["command"], "event.subscribe")
                agent.send(
                    connection,
                    {"id": subscribe["id"], "ok": True, "result": {}},
                )
                history = agent.receive(connection)
                self.assertEqual(history["command"], "event.history")
                agent.send(
                    connection,
                    {
                        "id": history["id"],
                        "ok": True,
                        "result": [
                            {
                                "event": "operation.finished",
                                "sequence": "99",
                                "data": {"operationId": "42", "outcome": "completed"},
                            }
                        ],
                    },
                )

            output = StringIO()
            with FakeAgent(path, handler), redirect_stdout(output):
                status = main(
                    ["--socket", os.fspath(path), "wait", "42"]
                )
            self.assertEqual(status, 0)
            self.assertEqual(json.loads(output.getvalue())["sequence"], "99")


if __name__ == "__main__":
    unittest.main()
