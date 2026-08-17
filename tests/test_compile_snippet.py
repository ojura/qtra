from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from compile_snippet import (
    BuildOracleError,
    derive_shared_object_command,
    load_database,
    select_entry,
)


class CompileSnippetTests(unittest.TestCase):
    def make_database(self, root: Path, *, arguments: bool = True) -> Path:
        source = root / "src" / "widget.cpp"
        source.parent.mkdir(parents=True)
        source.write_text("int widget() { return 1; }\n", encoding="utf-8")
        tokens = [
            "/usr/bin/c++",
            "-DAPP=1",
            "-I../include",
            "-std=gnu++20",
            "-march=x86-64-v3",
            "-O3",
            "-flto=auto",
            "-fno-fat-lto-objects",
            "-fuse-linker-plugin",
            "-fPIE",
            "-MMD",
            "-MF",
            "widget.d",
            "-MT",
            "widget.o",
            "-c",
            str(source),
            "-o",
            "widget.o",
        ]
        item = {
            "directory": str(root),
            "file": str(source),
            "output": "CMakeFiles/app.dir/src/widget.cpp.o",
        }
        if arguments:
            item["arguments"] = tokens
        else:
            import shlex

            item["command"] = shlex.join(tokens)
        path = root / "compile_commands.json"
        path.write_text(json.dumps([item]), encoding="utf-8")
        return path

    def test_load_select_and_transform_arguments(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            database = self.make_database(root)
            entry = select_entry(load_database(database), "src/widget.cpp")
            snippet = root / "snippet.cpp"
            snippet.write_text("extern \"C\" int x() { return 2; }\n", encoding="utf-8")
            output = root / "snippet.so"
            command = derive_shared_object_command(entry, snippet, output)

            self.assertEqual(command[0], "/usr/bin/c++")
            self.assertIn("-DAPP=1", command)
            self.assertIn("-I../include", command)
            self.assertIn("-std=gnu++20", command)
            self.assertIn("-march=x86-64-v3", command)
            self.assertIn("-Og", command)
            self.assertIn("-fPIC", command)
            self.assertIn("-fno-access-control", command)
            self.assertIn("-shared", command)
            self.assertNotIn("-O3", command)
            self.assertNotIn("-flto=auto", command)
            self.assertNotIn("-fno-fat-lto-objects", command)
            self.assertNotIn("-fuse-linker-plugin", command)
            self.assertNotIn("-fPIE", command)
            self.assertNotIn("-MMD", command)
            self.assertNotIn("widget.d", command)
            self.assertNotIn(str(entry.file), command)
            self.assertEqual(command[-2:], ["-o", str(output.resolve())])

    def test_command_string_database(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            database = self.make_database(root, arguments=False)
            entries = load_database(database)
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0].file.name, "widget.cpp")

    def test_unknown_context_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entries = load_database(self.make_database(root))
            with self.assertRaises(BuildOracleError):
                select_entry(entries, "does-not-exist.cpp")


if __name__ == "__main__":
    unittest.main()
