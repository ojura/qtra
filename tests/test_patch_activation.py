"""Replacing the step function over the socket, on a real build.

This is about the wiring: that patch.load, patch.activate, patch.status and
patch.rollback reach the machinery, and that a build carrying its own manifest
admits the write. What the admission rule decides for a given manifest is
settled by coverage_admission_selftest, which reads temporary files and needs
neither a display nor a build artifact.

Nothing here edits the build's manifest. The one this reads is the one the
build wrote, so being killed leaves nothing to put back.
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

# Which build to drive. ctest sets this to the configuration it is running, so
# release-lto and release-cet exercise their own binaries and not another
# configuration's. Direct invocation gets build/release.
#
# This chooses which binary the test launches and decides nothing the product
# does. The manifest each binary reads is still the one its own build wrote,
# beside it.
BUILD = Path(os.environ.get("RUNTIME_AGENT_TEST_BUILD_DIR", ROOT / "build" / "release"))
BINARY = BUILD / "qt_runtime_cube"
MANIFEST = BUILD / "coverage-manifest.json"
PATCH_MODULE = BUILD / "cube_patch_wobble.so"

HAS_DISPLAY = bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))


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
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                connection.connect(str(self.socket_path))
            except OSError:
                connection.close()
                time.sleep(0.05)
                continue
            connection.settimeout(20)
            return connection
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

    def ok(self, command: str, **params: object) -> dict:
        answer = self.call(command, **params)
        if not answer.get("ok"):
            raise AssertionError(f"{command} refused: {answer}")
        return answer["result"]

    def close(self) -> None:
        try:
            self.connection.close()
        finally:
            self.process.terminate()
            self.process.wait(10)


@unittest.skipUnless(
    BINARY.exists() and MANIFEST.exists() and PATCH_MODULE.exists() and HAS_DISPLAY,
    f"needs a display and a built, manifest-bearing {BUILD}",
)
class PatchActivation(unittest.TestCase):
    def setUp(self) -> None:
        # Named so a log says which binary ran. ctest hides this on a pass, so
        # it shows under -V or on failure; what makes each configuration drive
        # its own binary is RUNTIME_AGENT_TEST_BUILD_DIR above, not this line.
        print(f"driving {BINARY.resolve()}", flush=True)

        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.agent = Agent(Path(directory.name) / "agent.sock")
        self.addCleanup(self.agent.close)

    def test_activate_and_roll_back_a_replacement(self) -> None:
        module = self.agent.ok("patch.load", path=str(PATCH_MODULE))["moduleId"]

        before = self.agent.ok("patch.status")
        self.assertEqual(before["entryState"], "pristine")

        self.agent.ok("patch.activate", moduleId=module, acceptIncompleteCoverage=False)

        active = self.agent.ok("patch.status")
        self.assertEqual(active["entryState"], "replacement")
        self.assertEqual(active["mode"], "entry")
        # The write happened for a stated reason, and the reason is reported.
        self.assertTrue(active["quiescedBy"])
        self.assertTrue(active["coverage"]["allow"])

        self.agent.ok("patch.rollback")

        # The gateway stays once installed. Rolling back selects the original
        # through it, which is a store, and does not put the entry back.
        after = self.agent.ok("patch.status")
        self.assertEqual(after["entryState"], "original")
        self.assertEqual(after["gatewaySlot"], active["gatewaySlot"])


    def test_status_names_the_module_that_bound_through_the_host(self) -> None:
        """A module binding through patch_bind is what status reports.

        Reporting once read the adapter's own record of the protocol binding,
        which the host ABI path never wrote, so a snippet could replace the step
        function and leave status saying the built-in one was running.
        """
        snippet = BUILD / "agent_snippet_audio_pulse.so"
        if not snippet.exists():
            self.skipTest("built without PulseAudio, so no snippet binds through the host")

        module = self.agent.ok("snippet.load", path=str(snippet))["moduleId"]
        self.agent.ok("snippet.run", moduleId=module, executor="render", request={})

        # snippet.run returns an operation id and finishes later.
        for _ in range(100):
            status = self.agent.ok("patch.status")
            if status["entryState"] == "replacement":
                break
            time.sleep(0.05)

        self.assertEqual(status["entryState"], "replacement")
        self.assertEqual(status["mode"], "entry")
        self.assertEqual(status["moduleId"], str(module))


if __name__ == "__main__":
    unittest.main()
