"""The command set exists in two copies; this makes them disagree loudly.

dispatchRequest is an if-chain over command names and commandList() is a hand
written list of the same names, kept in sync by remembering to. Drift is cheap
to introduce and its cost is asymmetric: a command missing from the list still
works but cannot be discovered by a client that browses, and a name left in the
list after its handler goes answers unknown_command.

This does not fix the duplication — the honest fix is a single dispatch table
the list is derived from — but it removes the part that depends on memory. It
reads the source rather than a running agent so it costs nothing and runs in
the ordinary suite.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

AGENT_SOURCE = Path(__file__).resolve().parents[1] / "src" / "agent" / "runtime_agent.cpp"

DISPATCHED = re.compile(r'if \(command == QStringLiteral\("([a-z][a-zA-Z.]*)"\)')
COMMAND_NAME = re.compile(r'QStringLiteral\("([a-z][a-zA-Z.]*)"\)')


def command_list_body(source: str) -> str:
    start = source.index("QJsonArray RuntimeAgent::commandList() const")
    end = source.index("\n}\n", start)
    return source[start:end]


class ProtocolSurfaceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.source = AGENT_SOURCE.read_text(encoding="utf-8")

    def test_every_dispatched_command_is_listed(self) -> None:
        dispatched = set(DISPATCHED.findall(self.source))
        listed = set(COMMAND_NAME.findall(command_list_body(self.source)))
        self.assertTrue(dispatched, "found no dispatched commands; the pattern has rotted")
        missing = sorted(dispatched - listed)
        self.assertEqual(missing, [], f"dispatched but absent from help: {missing}")

    def test_every_listed_command_is_dispatched(self) -> None:
        dispatched = set(DISPATCHED.findall(self.source))
        listed = set(COMMAND_NAME.findall(command_list_body(self.source)))
        self.assertTrue(listed, "found no listed commands; the pattern has rotted")
        stale = sorted(listed - dispatched)
        self.assertEqual(stale, [], f"listed in help with no handler: {stale}")


if __name__ == "__main__":
    unittest.main()
