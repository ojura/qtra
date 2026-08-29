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

import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

# The same client agentctl.py drives the application with. Talking to the socket
# from here as well meant two answers to how a reply is framed, and the copy
# here was the worse one: it dropped events on the floor rather than holding
# them, and it read a closed socket as an empty read forever.
from agentctl import AgentClient, ProtocolError
from private_display import OwnDisplay, can_draw, child_environment

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
CAN_DRAW = can_draw()


def missingArtifacts() -> list[str]:
    """The build outputs these tests drive, and which of them are absent.

    Kept apart from CAN_DRAW because the two deserve opposite answers. A machine
    with no Xvfb cannot run these and skipping is honest. A build that did not
    produce its own binary is a broken build, and skipping there reports success
    for a run that exercised no application at all, which is what a validation
    is for.
    """
    return [str(path) for path in (BINARY, MANIFEST, PATCH_MODULE, BINDER)
            if not path.exists()]


def requireArtifacts() -> None:
    """Fails when the build did not produce what these tests drive."""
    absent = missingArtifacts()
    if absent:
        raise AssertionError(
            "this build did not produce " + ", ".join(absent)
            + ". These tests drive the application, so a build without it is a failure "
              "and not something to skip past")


DISPLAY_FOR_TESTS = OwnDisplay()


def setUpModule() -> None:
    """Starts a display of this module's own, or skips.

    Falling through to whatever display is around would put windows on
    somebody's screen at the moment something has gone wrong, which is the
    worst time for it.
    """
    if os.environ.get("RUNTIME_AGENT_TEST_DISPLAY") == "host":
        return
    if DISPLAY_FOR_TESTS.start() is None:
        raise unittest.SkipTest(
            "no display of our own could be started, and using the one this shell has "
            "must be asked for with RUNTIME_AGENT_TEST_DISPLAY=host"
        )


def tearDownModule() -> None:
    DISPLAY_FOR_TESTS.stop()


class Agent:
    """The demo, talking on a private socket."""

    def __init__(self, socket_path: Path) -> None:
        self.socket_path = socket_path
        # A real display, because the cube is a QOpenGLWidget and Qt's offscreen
        # platform has no support for one: it reports that, fails to compile the
        # shaders, and dies before the socket is useful.
        #
        # setUpModule has already skipped if there is no private display. This
        # raises rather than falling back, so a caller that goes around
        # setUpModule gets an error and not a window on somebody's screen.
        environment = child_environment(DISPLAY_FOR_TESTS)
        self.process = subprocess.Popen(
            [str(BINARY), "--agent-socket", str(socket_path)],
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            self.connection = self._connect()
        except BaseException:
            # The demo is already running. Failing out of the constructor
            # without this leaves it running, because nothing has been
            # registered to stop it yet.
            self.close()
            raise

    def _connect(self) -> AgentClient:
        # The client connects once. Retrying is this test's business, because
        # the demo is still starting and has not bound the socket yet, and only
        # the caller that launched it knows to give up when it dies instead.
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("the demo exited before it accepted a connection")
            client = AgentClient(str(self.socket_path), timeout=20)
            try:
                client.connect()
            # ConnectionError and TimeoutError are both OSError.
            except OSError:
                client.close()
                time.sleep(0.05)
                continue
            return client
        raise RuntimeError("the demo never accepted a connection")

    def call(self, command: str, **params: object) -> dict:
        """The whole reply, for a test whose subject is the refusal."""
        return self.connection.exchange(command, params)

    def ok(self, command: str, **params: object) -> dict:
        try:
            return self.connection.request(command, params)
        except ProtocolError as refusal:
            raise AssertionError(f"{command} refused: {refusal}") from refusal

    def close(self) -> None:
        """Stops the demo, and is safe to call more than once.

        Asking politely and waiting is not enough on its own: a demo that has
        wedged stays running, and the next thing to notice is somebody finding
        it hours later.
        """
        connection = getattr(self, "connection", None)
        if connection is not None:
            connection.close()
            self.connection = None
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(10)
        self.process = None


@unittest.skipUnless(CAN_DRAW, "no display can be had on this machine")
class PatchActivation(unittest.TestCase):
    def setUp(self) -> None:
        # Named so a log says which binary ran. ctest hides this on a pass, so
        # it shows under -V or on failure; what makes each configuration drive
        # its own binary is RUNTIME_AGENT_TEST_BUILD_DIR above, not this line.
        requireArtifacts()
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



@unittest.skipUnless(CAN_DRAW, "no display can be had on this machine")
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

        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        requireArtifacts()
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
