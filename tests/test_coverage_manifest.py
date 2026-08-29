"""The analyzer has to refuse what it cannot establish.

Every case here produced, or could produce, a report that says a function may be
replaced when nothing has shown that. A permissive answer from an admission
oracle is worse than no oracle, because it is acted on.
"""

import json
import pathlib
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
ANALYZER = REPO / "tools" / "analyze_coverage.py"


def build(sources: dict[str, str],
          flags: list[str],
          directory: pathlib.Path,
          target: str = "t",
          per_file_flags: dict[str, list[str]] | None = None) -> pathlib.Path:
    """Compile like CMake does, one object per source under CMakeFiles/<target>.dir.

    The layout matters. The analyzer finds the object a binary was built from by
    looking for that directory in the compile database, and a fixture that runs
    gcc straight from a temporary directory has no database at all, so every
    modality comes back unanswered and a test passes without exercising anything
    it names.
    """
    objects = []
    entries = []
    objdir = directory / "CMakeFiles" / f"{target}.dir"
    objdir.mkdir(parents=True, exist_ok=True)
    for name, text in sources.items():
        (directory / name).write_text(text)
        output = f"CMakeFiles/{target}.dir/{name}.o"
        file_flags = flags + (per_file_flags or {}).get(name, [])
        command = ["gcc", "-O2", *file_flags, "-c", name, "-o", output]
        subprocess.run(command, cwd=directory, check=True)
        objects.append(output)
        entries.append({"directory": str(directory), "file": str(directory / name),
                        "output": output, "command": " ".join(command)})

    (directory / "compile_commands.json").write_text(json.dumps(entries, indent=2))
    binary = directory / target
    subprocess.run(["gcc", *objects, "-o", target], cwd=directory, check=True)
    return binary


def analyze(binary: pathlib.Path, build_dir: pathlib.Path, **kwargs) -> dict:
    command = ["python3", str(ANALYZER), "--binary", str(binary),
               "--build-dir", str(build_dir), "--source-hint", "t.c"]
    for key, value in kwargs.items():
        flag = "--" + key.replace("_", "-")
        command += [flag] if value is True else [flag, str(value)]
    result = subprocess.run(command, capture_output=True, text=True, cwd=REPO)
    return json.loads(result.stdout)


PREPARED = ["-fpatchable-function-entry=20,0", "-fno-lto", "-fno-ipa-icf",
            "-fdump-ipa-clones"]


class CoverageAnalyzer(unittest.TestCase):
    def test_missing_target_is_refused(self):
        """A name absent from the binary has no aliases and no clones, which
        once read as every question answered."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build({"t.c": "int f(int x){return x+1;}\nint main(void){return f(1);}"},
                           PREPARED, directory)
            report = analyze(binary, directory, target="nothing_of_this_name")
            self.assertEqual(report["decision"], "refuse")
            self.assertIn("target symbol", report["unknown"])

    def test_unprepared_target_is_refused_on_its_own_ground(self):
        """One function having a reserved area says nothing about another. The
        prepared flag is applied to one file only, so the other really is
        unprepared and has to be refused for that reason and not another."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build(
                {"prepared.c": "__attribute__((noinline)) int prepared(int x){return x+1;}",
                 "plain.c": "int prepared(int);\nint bare(int x){return x*2;}\n"
                            "int main(void){return prepared(1)+bare(2);}"},
                ["-fno-lto", "-fno-ipa-icf", "-fdump-ipa-clones"], directory,
                per_file_flags={"prepared.c": ["-fpatchable-function-entry=20,0"]})
            report = analyze(binary, directory, target="bare", source_hint="plain.c")
            self.assertEqual(report["decision"], "refuse")
            self.assertIn("prepared entry for this target", report["unknown"])

    def test_prepared_target_in_the_same_binary_is_allowed(self):
        """The counterpart, so the refusal above is about this target and not
        about something wrong with the fixture."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build(
                {"prepared.c": "__attribute__((noinline)) int prepared(int x){return x+1;}",
                 "plain.c": "int prepared(int);\nint main(void){return prepared(1);}"},
                ["-fno-lto", "-fno-ipa-icf", "-fdump-ipa-clones"], directory,
                per_file_flags={"prepared.c": ["-fpatchable-function-entry=20,0"]})
            report = analyze(binary, directory, target="prepared",
                             source_hint="prepared.c")
            self.assertEqual(report["decision"], "allow", report["unknown"])

    def test_alias_is_refused(self):
        """Two names at one address means replacing one replaces both."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build(
                {"t.c": "__attribute__((noinline)) int alpha(int x){return x*3+1;}\n"
                        "int beta(int x) __attribute__((alias(\"alpha\")));\n"
                        "int main(void){return alpha(1)+beta(2);}"},
                PREPARED, directory)
            report = analyze(binary, directory, target="alpha")
            self.assertIn("beta", report["aliases"])
            self.assertTrue(any(s["what"] == "beta" for s in report["skipped"]))
            self.assertEqual(report["decision"], "refuse")

    def test_dump_belonging_to_another_object_cannot_answer(self):
        """Two targets can compile the same source, each with its own dump. The
        one beside this binary's object is the only one that describes it, and
        renaming it has to change the verdict."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build({"t.c": "__attribute__((noinline)) int f(int x){return x+1;}\n"
                                   "int main(void){return f(1);}"}, PREPARED, directory)
            before = analyze(binary, directory, target="f")
            self.assertEqual(before["decision"], "allow", before["unknown"])
            self.assertIsNotNone(before["cloneDump"])

            owning = pathlib.Path(before["cloneDump"])
            owning.rename(owning.parent / "belongs_to_something_else.ipa-clones")
            after = analyze(binary, directory, target="f")
            self.assertEqual(after["decision"], "refuse")
            self.assertIn("IPA clone dumps for the owning object", after["unknown"])

    def test_observed_domain_does_not_authorize(self):
        """Having seen one thread is not proof another never arrives."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build({"t.c": "__attribute__((noinline)) int f(int x){return x+1;}\n"
                                   "int main(void){return f(1);}"}, PREPARED, directory)
            observed = analyze(binary, directory, target="f",
                               caller_domain="only the main thread was seen",
                               caller_domain_strength="observed")
            self.assertFalse(observed["callerExecutionDomain"]["authorizesRequestBoundary"])
            declared = analyze(binary, directory, target="f",
                               caller_domain="the main thread only",
                               caller_domain_strength="declared")
            self.assertTrue(declared["callerExecutionDomain"]["authorizesRequestBoundary"])


if __name__ == "__main__":
    unittest.main()
