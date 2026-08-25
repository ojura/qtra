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

from agentctl import AgentClient, ProtocolError, hand_over, main, release_snippet


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


class DeferredEventTests(unittest.TestCase):
    def test_operation_finished_during_another_request_is_not_lost(self) -> None:
        """An operation can finish while an unrelated command is in flight.

        The event arrives on the same socket as the response. Discarding it left
        a later wait_for_operation waiting for something that had already
        happened, which times out and reads as the application hanging.
        """
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "agent.sock"

            def handler(agent: FakeAgent, connection: socket.socket) -> None:
                request = agent.receive(connection)
                # The completion arrives before the reply to the command that
                # happened to be in flight.
                agent.send(
                    connection,
                    {
                        "event": "operation.finished",
                        "data": {"operationId": "77", "outcome": "completed"},
                    },
                )
                agent.send(connection, {"id": request["id"], "ok": True, "result": {}})

            with FakeAgent(path, handler):
                with AgentClient(os.fspath(path), timeout=2) as client:
                    client.request("cube.state")
                    finished = client.wait_for_operation("77", timeout=2)
            self.assertEqual(finished["data"]["outcome"], "completed")

    def test_waiting_for_one_operation_keeps_the_others(self) -> None:
        """Draining must not consume what it passes over.

        Two operations can finish while one command is in flight. Waiting for
        the second must leave the first's completion available, or the fix for
        the dropped-event bug simply moves the drop somewhere else.
        """
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "agent.sock"

            def handler(agent: FakeAgent, connection: socket.socket) -> None:
                request = agent.receive(connection)
                for operation in ("10", "11"):
                    agent.send(
                        connection,
                        {
                            "event": "operation.finished",
                            "data": {"operationId": operation, "outcome": "completed"},
                        },
                    )
                agent.send(connection, {"id": request["id"], "ok": True, "result": {}})
                time.sleep(1.0)

            with FakeAgent(path, handler):
                with AgentClient(os.fspath(path), timeout=2) as client:
                    client.request("cube.state")
                    second = client.wait_for_operation("11", timeout=1)
                    first = client.wait_for_operation("10", timeout=1)
            self.assertEqual(second["data"]["operationId"], "11")
            self.assertEqual(first["data"]["operationId"], "10")

    def test_a_wait_does_not_consume_another_operations_completion(self) -> None:
        """The socket loop must retain what it reads past, not only the drain.

        Both completions arrive after the wait has started, so they come off the
        socket rather than out of the deferred queue. Waiting for the second
        must leave the first available.
        """
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "agent.sock"

            def handler(agent: FakeAgent, connection: socket.socket) -> None:
                for operation in ("20", "21"):
                    agent.send(
                        connection,
                        {
                            "event": "operation.finished",
                            "data": {"operationId": operation, "outcome": "completed"},
                        },
                    )
                time.sleep(1.0)

            with FakeAgent(path, handler):
                with AgentClient(os.fspath(path), timeout=2) as client:
                    second = client.wait_for_operation("21", timeout=1)
                    first = client.wait_for_operation("20", timeout=1)
            self.assertEqual(second["data"]["operationId"], "21")
            self.assertEqual(first["data"]["operationId"], "20")

    def test_unrelated_deferred_events_do_not_satisfy_a_wait(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "agent.sock"

            def handler(agent: FakeAgent, connection: socket.socket) -> None:
                request = agent.receive(connection)
                agent.send(
                    connection,
                    {
                        "event": "operation.finished",
                        "data": {"operationId": "1", "outcome": "completed"},
                    },
                )
                agent.send(connection, {"id": request["id"], "ok": True, "result": {}})
                # Hold the connection open so the wait below reaches its
                # timeout rather than an end of stream.
                time.sleep(1.0)

            with FakeAgent(path, handler):
                with AgentClient(os.fspath(path), timeout=1) as client:
                    client.request("cube.state")
                    with self.assertRaises(TimeoutError):
                        client.wait_for_operation("99", timeout=0.3)

class HandoverTests(unittest.TestCase):
    """How reload classifies what the outgoing generation did.

    The distinction these cover is the whole reason reload asks a module to
    release rather than sending it a payload and hoping: a module that cannot
    have installed anything must not read as a failure, because a handover with
    nothing to do would then stop the reload and leave a module resident on
    every retry.
    """

    def _client_raising(self, code: str) -> Any:
        class Client:
            def request(self, command: str, params: Any = None, on_event: Any = None) -> Any:
                raise ProtocolError(f"{code}: refused", code)

            def wait_for_operation(self, *args: Any, **kwargs: Any) -> Any:
                raise AssertionError("should not wait after a refused release")

        return Client()

    def test_declared_none_is_not_a_failure(self) -> None:
        outcome, _ = release_snippet(
            self._client_raising("no_release_declared"), "1", None, None, 1.0
        )
        self.assertEqual(outcome, "declared-none")

    def test_module_that_never_ran_is_not_a_failure(self) -> None:
        # A module loaded but never run cannot hold an install, so there is
        # nothing for a handover to undo. Reporting this as a failure wedged
        # every later reload, because the generation matched by name stayed the
        # newest one forever.
        outcome, _ = release_snippet(
            self._client_raising("no_recorded_executor"), "1", None, None, 1.0
        )
        self.assertEqual(outcome, "never-ran")

    def test_other_agent_errors_are_failures(self) -> None:
        outcome, _ = release_snippet(
            self._client_raising("recorded_target_gone"), "1", None, None, 1.0
        )
        self.assertEqual(outcome, "failed")

    def test_release_that_ran_and_failed_is_a_failure(self) -> None:
        class Client:
            def request(self, command: str, params: Any = None, on_event: Any = None) -> Any:
                return {"operationId": "7"}

            def wait_for_operation(self, *args: Any, **kwargs: Any) -> Any:
                return {"data": {"operationId": "7", "outcome": "failed"}}

        outcome, detail = release_snippet(Client(), "1", None, None, 1.0)
        self.assertEqual(outcome, "failed")
        self.assertIn("finished", detail)

    def test_declared_none_falls_back_to_the_payload(self) -> None:
        sent: list[str] = []

        class Client:
            def request(self, command: str, params: Any = None, on_event: Any = None) -> Any:
                sent.append(command)
                if command == "snippet.release":
                    raise ProtocolError("no_release_declared: none", "no_release_declared")
                return {"operationId": "9"}

            def wait_for_operation(self, *args: Any, **kwargs: Any) -> Any:
                return {"data": {"operationId": "9", "outcome": "completed"}}

        result = hand_over(Client(), {"id": "1"}, None, None, '{"restore": true}', 1.0)
        self.assertEqual(result["outcome"], "payload-sent")
        self.assertEqual(result["route"], "handover-request")
        self.assertEqual(sent, ["snippet.release", "snippet.run"])

    def test_never_attempted_is_not_a_failure_and_sends_no_payload(self) -> None:
        """no_recorded_executor now means the module was never run at all.

        The agent resolves the executor from the last successful run, then from
        the last attempt, and only answers this when there was neither. A module
        that has never run cannot have installed anything, so there is nothing
        for a handover to undo and nothing to send a payload about. The case
        that used to hide here, installed and then failed, resolves to the
        executor that attempt used, and never reaches the client.
        """
        sent: list[Any] = []

        class Client:
            def request(self, command: str, params: Any = None, on_event: Any = None) -> Any:
                sent.append((command, (params or {}).get("executor")))
                raise ProtocolError("no_recorded_executor: never run", "no_recorded_executor")

            def wait_for_operation(self, *args: Any, **kwargs: Any) -> Any:
                raise AssertionError("nothing should be awaited")

        result = hand_over(Client(), {"id": "1"}, None, None, '{"restore": true}', 1.0)
        self.assertEqual(result["outcome"], "never-ran")
        self.assertEqual(sent, [("snippet.release", None)])
