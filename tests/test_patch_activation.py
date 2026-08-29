"""Replacing the step function over the socket, on a real build.

This is about the wiring: that patch.load, patch.activate, patch.status and
patch.rollback reach the machinery, that a build carrying its own manifest
admits the write, and that the window and the socket give one answer about what
the entry reaches.

What the admission rule decides for a given manifest, and what stands once one
has been read, are settled by coverage_admission_selftest, which writes its
inputs into a temporary directory and needs neither a display nor a build
artifact.

Nothing here edits the build's manifest. The one this reads is the one the
build wrote, so being killed leaves nothing to put back.
"""

from __future__ import annotations

import json
import os
import shutil
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
BINDER = BUILD / "agent_snippet_bind_probe.so"

# Asked at import, before the display exists, so this is whether one can be had.
CAN_DRAW = bool(
    os.environ.get("DISPLAY")
    or os.environ.get("WAYLAND_DISPLAY")
    or (os.environ.get("RUNTIME_AGENT_TEST_DISPLAY") != "host"
        and shutil.which("Xvfb") is not None)
)

# Where the demo is shown.
#
# On a display of its own by default, so running these does not put windows on
# somebody's screen while they are working. Set RUNTIME_AGENT_TEST_DISPLAY=host
# to use the display this shell already has, which is what to do when watching
# the cube is the point.
#
# The server is started here and stopped here, and the demo is launched directly
# against it. Wrapping each launch in xvfb-run instead would leave a wrapper
# between this and the process it has to stop, and killing the wrapper leaves
# the demo and the server behind.
class OwnDisplay:
    def __init__(self) -> None:
        self.number: str | None = None
        self.server: subprocess.Popen | None = None

    def start(self) -> str | None:
        if os.environ.get("RUNTIME_AGENT_TEST_DISPLAY") == "host":
            return None
        if shutil.which("Xvfb") is None:
            return None
        for candidate in range(90, 130):
            if Path(f"/tmp/.X11-unix/X{candidate}").exists():
                continue
            self.server = subprocess.Popen(
                ["Xvfb", f":{candidate}", "-screen", "0", "1280x800x24",
                 "+extension", "GLX", "+render", "-noreset"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if Path(f"/tmp/.X11-unix/X{candidate}").exists():
                    self.number = f":{candidate}"
                    return self.number
                if self.server.poll() is not None:
                    break
                time.sleep(0.05)
            self.stop()
        return None

    def stop(self) -> None:
        if self.server is not None:
            self.server.terminate()
            try:
                self.server.wait(10)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(5)
            self.server = None
        self.number = None


DISPLAY_FOR_TESTS = OwnDisplay()


def setUpModule() -> None:
    DISPLAY_FOR_TESTS.start()


def tearDownModule() -> None:
    DISPLAY_FOR_TESTS.stop()


def has_somewhere_to_draw() -> bool:
    return bool(
        DISPLAY_FOR_TESTS.number
        or os.environ.get("DISPLAY")
        or os.environ.get("WAYLAND_DISPLAY")
    )


class Agent:
    """The demo, talking on a private socket."""

    def __init__(self, socket_path: Path) -> None:
        self.socket_path = socket_path
        # A real display, because the cube is a QOpenGLWidget and Qt's offscreen
        # platform has no support for one: it reports that, fails to compile the
        # shaders, and dies before the socket is useful.
        environment = dict(os.environ)
        if DISPLAY_FOR_TESTS.number is not None:
            environment["DISPLAY"] = DISPLAY_FOR_TESTS.number
            # A virtual display has no GPU behind it.
            environment.setdefault("LIBGL_ALWAYS_SOFTWARE", "1")
        self.process = subprocess.Popen(
            [str(BINARY), "--agent-socket", str(socket_path)],
            env=environment,
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
    BINARY.exists() and MANIFEST.exists() and PATCH_MODULE.exists() and CAN_DRAW,
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



@unittest.skipUnless(
    BINARY.exists() and MANIFEST.exists() and PATCH_MODULE.exists() and CAN_DRAW,
    f"needs a display and a built, manifest-bearing {BUILD}",
)
class MixedGenerations(unittest.TestCase):
    """What the entry reaches, said twice, through a stack bound both ways.

    The window's label and patch.status answer one question. They were written
    from separate places, so releasing a host generation could leave the window
    naming the module that had just let go, and a protocol rollback left it
    naming a module that no longer ran.

    The binder is always built and needs nothing beyond the host ABI, so these
    run wherever the demo does.
    """

    def setUp(self) -> None:
        if not BINDER.exists():
            self.skipTest("the binding probe was not built")
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        print(f"driving {BINARY.resolve()}", flush=True)
        self.agent = Agent(Path(directory.name) / "agent.sock")
        self.addCleanup(self.agent.close)

    def label(self) -> str:
        # object.get answers with the value itself.
        return self.agent.ok("object.get", objectName="cubeView", property="activePatch")

    def module_names(self) -> dict:
        return {m["id"]: m["name"] for m in self.agent.ok("module.list")}

    def assert_agrees(self, where: str) -> dict:
        """The label names exactly the module status says is running.

        Checking only that it is not "builtin" would pass while the label named
        a module that had just let go, which is the defect.
        """
        status = self.agent.ok("patch.status")
        label = self.label()
        if status["mode"] == "builtin":
            self.assertEqual(label, "builtin", where)
        else:
            expected = self.module_names().get(status["moduleId"])
            self.assertIsNotNone(expected, f"{where}: status named an unknown module")
            self.assertEqual(label, f"entry: {expected}", where)
        return status

    def bind_through_host(self) -> str:
        module = self.agent.ok("snippet.load", path=str(BINDER))["moduleId"]
        self.agent.ok("snippet.run", moduleId=module, executor="gui", request={})
        self.wait_for(module)
        return module

    def wait_for(self, module: str) -> None:
        for _ in range(100):
            if self.agent.ok("patch.status")["moduleId"] == module:
                return
            time.sleep(0.05)

    def activate_protocol(self) -> str:
        module = self.agent.ok("patch.load", path=str(PATCH_MODULE))["moduleId"]
        self.agent.ok("patch.activate", moduleId=module, acceptIncompleteCoverage=False)
        return module

    def test_protocol_then_host_then_release_the_host_one(self) -> None:
        self.assert_agrees("before anything")
        protocol = self.activate_protocol()
        self.assertEqual(self.assert_agrees("after the protocol bind")["moduleId"], protocol)

        host = self.bind_through_host()
        self.assertEqual(self.assert_agrees("with both bound")["moduleId"], host)

        # Releasing the selected host generation reveals the protocol one, which
        # is where the label used to keep naming the module that let go.
        self.agent.ok("snippet.run", moduleId=host, executor="gui",
                      request={"release": True})
        self.wait_for(protocol)
        self.assertEqual(
            self.assert_agrees("after releasing the host binding")["moduleId"], protocol
        )

        self.agent.ok("patch.rollback")
        final = self.assert_agrees("after rolling back")
        self.assertEqual(final["mode"], "builtin")
        self.assertEqual(final["entryState"], "original")

    def test_host_then_protocol_then_roll_the_protocol_one_back(self) -> None:
        """The other order, where rollback reveals a host binding underneath.

        This is the one the protocol's own rollback never relabelled for: it
        zeroed its fields and returned, so the window named a module that had
        stopped running.
        """
        host = self.bind_through_host()
        self.assertEqual(self.assert_agrees("after the host bind")["moduleId"], host)

        protocol = self.activate_protocol()
        self.assertEqual(self.assert_agrees("with both bound")["moduleId"], protocol)

        self.agent.ok("patch.rollback")
        self.wait_for(host)
        revealed = self.assert_agrees("after rolling the protocol binding back")
        self.assertEqual(revealed["moduleId"], host)
        self.assertEqual(revealed["mode"], "entry")

    def test_releasing_a_binding_that_is_not_selected_changes_nothing(self) -> None:
        host = self.bind_through_host()
        protocol = self.activate_protocol()
        self.assertEqual(self.assert_agrees("with both bound")["moduleId"], protocol)

        # The host binding is underneath, so letting it go leaves the selection
        # alone. Releasing out of order is safe, and nothing should move.
        self.agent.ok("snippet.run", moduleId=host, executor="gui",
                      request={"release": True})
        after = self.assert_agrees("after releasing the buried binding")
        self.assertEqual(after["moduleId"], protocol)
        self.assertEqual(after["mode"], "entry")


if __name__ == "__main__":
    unittest.main()
