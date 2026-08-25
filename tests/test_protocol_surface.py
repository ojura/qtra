"""The command set is written down once, in commands().

It used to exist twice — an if-chain in dispatchRequest and a hand-written list
in commandList() — kept in sync by remembering to, with an asymmetric cost when
that failed: a command missing from the list still worked but could not be
discovered by a client that browses, and a name left behind after its handler
went answered unknown_command.

There is no second copy to compare against now, so these guard the property
that replaced it: dispatch selects on nothing but the table, and help derives
from the table rather than restating it. Both are the ways the duplication
could come back.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

AGENT_SOURCE = Path(__file__).resolve().parents[1] / "src" / "agent" / "runtime_agent.cpp"

TABLE_ENTRY = re.compile(r'\{"([a-z][a-zA-Z.]*)", &RuntimeAgent::(handle[A-Za-z]+)\}')
COMMAND_COMPARISON = re.compile(r'command == QStringLiteral\("([a-z][a-zA-Z.]*)"\)')


class ProtocolSurfaceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.source = AGENT_SOURCE.read_text(encoding="utf-8")

    def test_the_table_is_populated(self) -> None:
        entries = TABLE_ENTRY.findall(self.source)
        self.assertGreater(len(entries), 30, "the table pattern has rotted or the table shrank")
        names = [name for name, _ in entries]
        self.assertEqual(sorted(names), sorted(set(names)), "a command is in the table twice")

    def test_nothing_dispatches_on_a_command_name_outside_the_table(self) -> None:
        """A special case added beside the table is the duplication returning.

        It would work, and help would not mention it — the exact asymmetry the
        table removed.
        """
        stray = COMMAND_COMPARISON.findall(self.source)
        self.assertEqual(stray, [], f"command names compared outside the table: {stray}")

    def test_help_derives_from_the_table(self) -> None:
        start = self.source.index("QJsonArray RuntimeAgent::commandList() const")
        body = self.source[start:self.source.index("\n}\n", start)]
        self.assertIn("commands()", body, "commandList no longer reads the table")
        self.assertNotIn(
            "QStringLiteral(", body,
            "commandList names a command directly, which is the second copy coming back",
        )

    def test_every_handler_named_in_the_table_is_defined(self) -> None:
        for name, handler in TABLE_ENTRY.findall(self.source):
            with self.subTest(command=name):
                self.assertIn(f"void RuntimeAgent::{handler}(", self.source)


if __name__ == "__main__":
    unittest.main()
