"""The order the host asks the manifest's questions in.

Three questions decide whether the entry may be written, and they are not
interchangeable. Whether the manifest is about this binary and this function,
and whether the recorded claim about which threads reach it supports writing
while the process runs, are settled before acceptIncompleteCoverage is looked
at. That flag means one thing: the caller will take a replacement some callers
do not reach.

These drive the real executable over its socket, because the order lives in the
host and a test of the analyzer alone would not see it. The build's manifest is
edited in place and put back afterwards; a rebuild regenerates it in any case.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "release"
BINARY = BUILD / "qt_runtime_cube"
MANIFEST = BUILD / "coverage-manifest.json"
PATCH_MODULE = BUILD / "cube_patch_wobble.so"


class Agent:
    """The demo, talking on a private socket."""

    def __init__(self, socket_path: Path) -> None:
        self.socket_path = socket_path
        # A real display, because the cube is a QOpenGLWidget and Qt's offscreen
        # platform has no support for one: it reports that, fails to compile the
        # shaders, and dies before the socket is useful.
        self.process = subprocess.Popen(
            [str(BINARY), "--agent-socket", str(socket_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.connection = self._connect()
        self.next_id = 0

    def _connect(self) -> socket.socket:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("the demo exited before it accepted a connection")
            try:
                connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                connection.connect(str(self.socket_path))
                connection.settimeout(20)
                return connection
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("the demo never accepted a connection")

    def call(self, command: str, **params: object) -> dict:
        self.next_id += 1
        request = {"id": self.next_id, "command": command, "params": params}
        self.connection.sendall((json.dumps(request) + "\n").encode())
        buffer = b""
        while True:
            buffer += self.connection.recv(65536)
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                message = json.loads(line)
                # Events share the stream and are not answers to this request.
                if message.get("id") == self.next_id:
                    return message

    def close(self) -> None:
        try:
            self.connection.close()
        finally:
            self.process.terminate()
            self.process.wait(10)


HAS_DISPLAY = bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))


@unittest.skipUnless(
    BINARY.exists() and MANIFEST.exists() and PATCH_MODULE.exists() and HAS_DISPLAY,
    "needs a display and a built, manifest-bearing build/release",
)
class AdmissionOrder(unittest.TestCase):
    def setUp(self) -> None:
        self.original = MANIFEST.read_bytes()
        self.addCleanup(MANIFEST.write_bytes, self.original)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def write_manifest(self, edit) -> None:
        report = json.loads(self.original)
        edit(report)
        MANIFEST.write_text(json.dumps(report, indent=2))

    def activate(self, accept: bool) -> dict:
        agent = Agent(Path(self.directory.name) / "agent.sock")
        self.addCleanup(agent.close)
        loaded = agent.call("patch.load", path=str(PATCH_MODULE))
        self.assertTrue(loaded.get("ok"), loaded)
        return agent.call(
            "patch.activate",
            moduleId=loaded["result"]["moduleId"],
            acceptIncompleteCoverage=accept,
        )

    def assert_pristine_refusal(self, answer: dict) -> str:
        self.assertFalse(answer.get("ok"), f"expected a refusal, got {answer}")
        return answer["error"]["message"]

    def unauthorize_domain(self, report: dict) -> None:
        report["callerExecutionDomain"].update(
            {"strength": "observed", "authorizesRequestBoundary": False}
        )

    def test_flag_does_not_accept_an_unauthorized_domain(self) -> None:
        """A domain that was only observed refuses even with the flag set.

        Coverage is incomplete here too, so the flag has something to accept and
        the question is which answer wins. Whether some callers keep running the
        original is about the effect. Whether the bytes may be written at all
        while the process runs is a different question, and nobody answers it by
        not knowing.

        With coverage left complete this passes whatever the order is, because
        nothing reaches the flag. That made it worth nothing as a test.
        """

        def edit(report: dict) -> None:
            self.unauthorize_domain(report)
            report["coverage"] = "incomplete"

        self.write_manifest(edit)
        message = self.assert_pristine_refusal(self.activate(accept=True))
        self.assertIn("which threads reach this function", message)
        self.assertIn("observed", message)

    def test_an_unauthorized_domain_refuses_complete_coverage_too(self) -> None:
        """The same refusal where the flag has nothing to accept."""
        self.write_manifest(self.unauthorize_domain)
        message = self.assert_pristine_refusal(self.activate(accept=False))
        self.assertIn("which threads reach this function", message)

    def test_flag_does_not_accept_a_manifest_for_another_build(self) -> None:
        """A verdict about another binary is not about this one's entry."""
        self.write_manifest(lambda report: report.update({"buildId": "00" * 20}))
        message = self.assert_pristine_refusal(self.activate(accept=True))
        self.assertIn("different binary", message)

    def test_flag_does_not_accept_a_manifest_for_another_function(self) -> None:
        self.write_manifest(lambda report: report.update({"target": "some_other_function"}))
        message = self.assert_pristine_refusal(self.activate(accept=True))
        self.assertIn("some_other_function", message)

    def test_incomplete_coverage_refuses_without_the_flag(self) -> None:
        self.write_manifest(lambda report: report.update({"coverage": "incomplete"}))
        message = self.assert_pristine_refusal(self.activate(accept=False))
        self.assertIn("coverage is incomplete", message)

    def test_incomplete_coverage_is_what_the_flag_accepts(self) -> None:
        """The one thing it means, with the other two questions answered."""
        self.write_manifest(lambda report: report.update({"coverage": "incomplete"}))
        answer = self.activate(accept=True)
        self.assertTrue(answer.get("ok"), answer)


if __name__ == "__main__":
    unittest.main()
