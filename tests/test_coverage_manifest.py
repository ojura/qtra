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


def build(source: str, flags: list[str], directory: pathlib.Path) -> pathlib.Path:
    (directory / "t.c").write_text(source)
    binary = directory / "t"
    subprocess.run(["gcc", "-O2", *flags, str(directory / "t.c"), "-o", str(binary)],
                   cwd=directory, check=True)
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
        """A name that is not in the binary has no aliases and no clones, which
        once read as every question answered."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build("int f(int x){return x+1;}\nint main(void){return f(1);}",
                           PREPARED, directory)
            report = analyze(binary, directory, target="nothing_of_this_name")
            self.assertEqual(report["decision"], "refuse")
            self.assertIn("target symbol", report["unknown"])

    def test_unprepared_target_is_refused(self):
        """Another function having a reserved area says nothing about this one."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build(
                "__attribute__((noinline)) int prepared(int x){return x+1;}\n"
                "int main(void){return prepared(1);}", PREPARED, directory)
            report = analyze(binary, directory, target="main")
            self.assertEqual(report["decision"], "refuse")

    def test_alias_is_refused_unless_accepted(self):
        """Two names at one address means replacing one replaces both."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build(
                "__attribute__((noinline)) int alpha(int x){return x*3+1;}\n"
                "int beta(int x) __attribute__((alias(\"alpha\")));\n"
                "int main(void){return alpha(1)+beta(2);}", PREPARED, directory)
            report = analyze(binary, directory, target="alpha")
            self.assertIn("beta", report["aliases"])
            self.assertTrue(any(s["what"] == "beta" for s in report["skipped"]))

    def test_absent_clone_dumps_are_not_an_answer(self):
        """A build that recorded nothing has not been asked the question."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            unprobed = [f for f in PREPARED if f != "-fdump-ipa-clones"]
            binary = build("__attribute__((noinline)) int f(int x){return x+1;}\n"
                           "int main(void){return f(1);}", unprobed, directory)
            report = analyze(binary, directory, target="f")
            self.assertEqual(report["coverage"], "unknown")
            self.assertTrue(any("clone dumps" in u for u in report["unknown"]), report["unknown"])

    def test_unrelated_dump_cannot_answer_for_the_owning_object(self):
        """More than one target can compile the same source, and each gets its
        own dump. A dump belonging to a different object says nothing about the
        translation unit that is actually in this binary."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build("__attribute__((noinline)) int f(int x){return x+1;}\n"
                           "int main(void){return f(1);}", PREPARED, directory)
            # A dump exists in the tree, but not the one beside this object.
            for dump in directory.glob("*.ipa-clones"):
                dump.rename(directory / "somebody_elses.ipa-clones")
            report = analyze(binary, directory, target="f")
            self.assertEqual(report["decision"], "refuse")
            self.assertTrue(any("owning object" in u for u in report["unknown"]),
                            report["unknown"])

    def test_observed_domain_does_not_authorize(self):
        """Having seen one thread is not proof another never arrives."""
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            binary = build("__attribute__((noinline)) int f(int x){return x+1;}\n"
                           "int main(void){return f(1);}", PREPARED, directory)
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
