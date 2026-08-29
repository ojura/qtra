"""The command set is written down once, and dispatch selects on nothing else.

It used to exist twice, an if-chain in dispatchRequest and a hand-written list
in commandList(), kept in sync by remembering to, with an asymmetric cost when
that failed: a command missing from the list still worked but could not be
discovered by a client that browses, and a name left behind after its handler
went answered unknown_command.

There is no second copy to compare against now. The set is written in two
places, but they are two different sets: registerCoreCommands() in the agent
holds what the agent answers itself, and registerCubeProtocol() holds what this
application added. Both end up in one table, so both answer to one dispatch and
one help, and the checks below read both.

The static checks guard the structure. The live ones drive a real build and
check that the two refusals still happen and still say what they refused, which
no amount of reading the source can establish.
"""

from __future__ import annotations

import os
import re
import tempfile
import unittest
from pathlib import Path

from test_patch_activation import BINARY, CAN_DRAW, DISPLAY_FOR_TESTS, Agent

ROOT = Path(__file__).resolve().parents[1]
CORE_SOURCE = ROOT / "src" / "agent" / "runtime_agent.cpp"
CORE_HEADER = ROOT / "src" / "agent" / "runtime_agent.h"
APPLICATION_SOURCE = ROOT / "src" / "cube_protocol.cpp"

# Both files register through a local add(), so one pattern reads either.
REGISTRATION = re.compile(r'add\("([a-z][a-zA-Z.]*)", \{([^}]*)\}, ')
CORE_HANDLER = re.compile(r'add\("([a-z][a-zA-Z.]*)", \{[^}]*\}, &RuntimeAgent::(handle[A-Za-z]+)\)')
COMMAND_COMPARISON = re.compile(r'command == QStringLiteral\("([a-z][a-zA-Z.]*)"\)')


def names(source: str) -> list[str]:
    return [name for name, _parameters in REGISTRATION.findall(source)]


class ProtocolSurfaceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.core = CORE_SOURCE.read_text(encoding="utf-8")
        self.core_header = CORE_HEADER.read_text(encoding="utf-8")
        self.application = APPLICATION_SOURCE.read_text(encoding="utf-8")

    def test_the_core_table_is_populated(self) -> None:
        found = names(self.core)
        self.assertGreater(len(found), 25, "the pattern has rotted or the core table shrank")
        self.assertEqual(sorted(found), sorted(set(found)), "a command is registered twice")

    def test_the_application_registers_its_own(self) -> None:
        found = names(self.application)
        self.assertGreater(len(found), 5, "the pattern has rotted or the cube's commands shrank")
        self.assertEqual(sorted(found), sorted(set(found)), "a command is registered twice")

    def test_no_name_is_claimed_by_both(self) -> None:
        """The second registration is refused at startup, so it is a crash, not a surprise."""
        both = set(names(self.core)) & set(names(self.application))
        self.assertEqual(both, set(), f"registered in the core and by the application: {both}")

    def test_the_core_does_not_name_the_application(self) -> None:
        """What this whole split is for: a second application would carry the cube.

        The agent keeps the socket, the table, the refusals, the event history and
        the object registry, and an application that wanted those used to have to
        bring a CubeWidget to get them.
        """
        for where, source in (("runtime_agent.cpp", self.core), ("runtime_agent.h", self.core_header)):
            with self.subTest(file=where):
                named = [
                    f"{where}:{number}: {line.strip()}"
                    for number, line in enumerate(source.splitlines(), start=1)
                    if "cube" in line.lower()
                ]
                self.assertEqual(named, [], "the agent names this demo again")

    def test_nothing_dispatches_on_a_command_name_outside_the_table(self) -> None:
        """A special case added beside the table is the duplication returning.

        It would work, and help would not mention it, which is the exact asymmetry the
        table removed.
        """
        for where, source in (("core", self.core), ("application", self.application)):
            with self.subTest(file=where):
                stray = COMMAND_COMPARISON.findall(source)
                self.assertEqual(stray, [], f"command names compared outside the table: {stray}")

    def test_help_derives_from_the_table(self) -> None:
        start = self.core.index("QJsonArray RuntimeAgent::commandList() const")
        body = self.core[start:self.core.index("\n}\n", start)]
        self.assertIn("m_commands", body, "commandList no longer reads the table")
        self.assertNotIn(
            "QStringLiteral(", body,
            "commandList names a command directly, which is the second copy coming back",
        )

    def test_every_core_handler_named_in_the_table_is_defined(self) -> None:
        for name, handler in CORE_HANDLER.findall(self.core):
            with self.subTest(command=name):
                self.assertIn(f"void RuntimeAgent::{handler}(", self.core)


class LiveDispatch(unittest.TestCase):
    """The refusals, against a running build.

    An application's commands go into the same table as the core's, so they get
    the same dispatch and the same two refusals. That is a runtime claim: the
    source says where the entries come from and nothing more.
    """

    agent: Agent

    @classmethod
    def setUpClass(cls) -> None:
        if not (BINARY.exists() and CAN_DRAW):
            raise unittest.SkipTest(f"needs a display and a built {BINARY}")
        # A display of this class's own, on the same policy the rest of the
        # live tests keep: falling through to whatever display is around would
        # put a window on somebody's screen.
        if os.environ.get("RUNTIME_AGENT_TEST_DISPLAY") != "host" and DISPLAY_FOR_TESTS.number is None:
            if DISPLAY_FOR_TESTS.start() is None:
                raise unittest.SkipTest(
                    "no display of our own could be started, and using the one this shell "
                    "has must be asked for with RUNTIME_AGENT_TEST_DISPLAY=host"
                )
            cls.addClassCleanup(DISPLAY_FOR_TESTS.stop)

        print(f"driving {BINARY.resolve()}", flush=True)
        directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(directory.cleanup)
        cls.agent = Agent(Path(directory.name) / "agent.sock")
        cls.addClassCleanup(cls.agent.close)

    def test_an_unknown_command_is_refused(self) -> None:
        answer = self.agent.call("cube.levitate")
        self.assertFalse(answer["ok"])
        self.assertEqual(answer["error"]["code"], "unknown_command")

    def test_a_parameter_the_command_does_not_read_is_refused(self) -> None:
        """And says what it does read, so a typo is one round trip and not a hunt."""
        answer = self.agent.call("cube.speed", degreesPerSekond=30)
        self.assertFalse(answer["ok"])
        self.assertEqual(answer["error"]["code"], "unknown_parameter")
        self.assertIn("degreesPerSekond", answer["error"]["message"])
        self.assertIn("it reads degreesPerSecond", answer["error"]["message"])

    def test_help_lists_the_core_and_the_application_together(self) -> None:
        listed = self.agent.ok("help")
        self.assertIn("object.list", listed, "a command the agent answers itself is missing")
        self.assertIn("cube.state", listed, "a command the application registered is missing")

    def test_a_command_the_application_registered_answers(self) -> None:
        """Listed is not the same as reachable, so ask one and read the answer."""
        state = self.agent.ok("cube.state")
        self.assertIn("angleDegrees", state)
        self.assertIn("frameIndex", state)


if __name__ == "__main__":
    unittest.main()
