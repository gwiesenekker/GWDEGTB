#!/usr/bin/env python3
"""Small, deterministic driver tests; no large EGTB or host-wide load."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent
STUB = """#!/usr/bin/env python3
import os
from pathlib import Path
import sys

args = sys.argv[1:]
checkpoint = Path(args[args.index('--checkpoint') + 1])
counts = tuple(map(int, args[-4:]))
name = f'{counts[0]}wX-{counts[1]}wO-{counts[2]}bX-{counts[3]}bO'
with Path(os.environ['CALLS']).open('a') as calls:
    calls.write(name + '\\n')
if os.environ.get('FAIL_MATERIAL') == name:
    print('VERIFICATION FAILED: test failure')
    sys.exit(1)
if os.environ.get('INTERRUPT_MATERIAL') == name:
    sys.exit(130)
if not checkpoint.exists():
    checkpoint.write_text('partial')
    print('verified through index 1024/2048')
    sys.exit(2)
print(f'VERIFIED {name}.dtm: positions=2048 DTM-values-checked=4096')
sys.exit(0)
"""


class VerifyAllTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="verify-all-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / "data"
        self.data.mkdir()
        self.state = self.root / "state"
        self.stub = self.root / "stub.py"
        self.stub.write_text(STUB)
        self.stub.chmod(0o755)
        self.calls = self.root / "calls.txt"
        for name in ("4wX-0wO-4bX-0bO", "4wX-0wO-3bX-1bO"):
            (self.data / f"{name}.dtm").write_bytes(b"fixture")

    def run_driver(self, *extra, fail_material=None):
        environment = os.environ.copy()
        environment["CALLS"] = str(self.calls)
        if fail_material:
            environment["FAIL_MATERIAL"] = fail_material
        return subprocess.run(
            [sys.executable, str(ROOT / "verify_all.py"),
             "--directory", str(self.data), "--state-directory", str(self.state),
             "--verifier", str(self.stub), "--families", "4x4", "--limit", "2",
             *extra], capture_output=True, text=True, env=environment, check=False)

    def test_resume_and_report(self):
        first = self.run_driver("--max-chunks", "1")
        self.assertEqual(first.returncode, 2, first.stderr)
        report = (self.state / "summary.tsv").read_text()
        self.assertIn("4wX-0wO-4bX-0bO\tPARTIAL\t1024\t2048", report)
        self.assertIn("4wX-0wO-3bX-1bO\tNOT_CHECKED", report)
        second = self.run_driver("--max-chunks", "1")
        self.assertEqual(second.returncode, 2, second.stderr)
        report = (self.state / "summary.tsv").read_text()
        self.assertIn("4wX-0wO-4bX-0bO\tVERIFIED\t2048\t2048", report)
        self.assertIn("4wX-0wO-3bX-1bO\tPARTIAL\t1024\t2048", report)
        third = self.run_driver()
        self.assertEqual(third.returncode, 0, third.stderr)
        report = (self.state / "summary.tsv").read_text()
        self.assertEqual(report.count("\tVERIFIED\t"), 2)
        self.assertEqual(self.calls.read_text().splitlines(), [
            "4wX-0wO-4bX-0bO", "4wX-0wO-4bX-0bO", "4wX-0wO-3bX-1bO",
            "4wX-0wO-4bX-0bO", "4wX-0wO-3bX-1bO"])
        self.assertEqual(len(list(self.state.glob("*.log"))), 5)

    def test_failure_stops_before_dependents(self):
        result = self.run_driver(fail_material="4wX-0wO-4bX-0bO")
        self.assertEqual(result.returncode, 1)
        self.assertIn("\tFAILED\t", (self.state / "summary.tsv").read_text())
        self.assertEqual(self.calls.read_text().splitlines(), ["4wX-0wO-4bX-0bO"])

    def test_interrupted_run_is_not_verified(self):
        environment = os.environ.copy()
        environment["CALLS"] = str(self.calls)
        environment["INTERRUPT_MATERIAL"] = "4wX-0wO-4bX-0bO"
        result = subprocess.run(
            [sys.executable, str(ROOT / "verify_all.py"),
             "--directory", str(self.data), "--state-directory", str(self.state),
             "--verifier", str(self.stub), "--families", "4x4", "--limit", "2"],
            capture_output=True, text=True, env=environment, check=False)
        self.assertEqual(result.returncode, 130)
        self.assertIn("\tINTERRUPTED\t", (self.state / "summary.tsv").read_text())

    def test_changed_inputs_are_not_accepted(self):
        mutating_stub = self.stub.read_text().replace(
            "if not checkpoint.exists():",
            "if os.environ.get('MUTATE'):\n"
            "    Path(os.environ['MUTATE']).write_bytes(b'changed')\n"
            "if not checkpoint.exists():")
        self.stub.write_text(mutating_stub)
        environment = os.environ.copy()
        environment["CALLS"] = str(self.calls)
        environment["MUTATE"] = str(self.data / "4wX-0wO-3bX-1bO.dtm")
        result = subprocess.run(
            [sys.executable, str(ROOT / "verify_all.py"),
             "--directory", str(self.data), "--state-directory", str(self.state),
             "--verifier", str(self.stub), "--families", "4x4", "--limit", "2"],
            capture_output=True, text=True, env=environment, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("INPUTS_CHANGED", (self.state / "summary.tsv").read_text())

    def test_order_and_missing_file(self):
        dry = self.run_driver("--dry-run")
        self.assertEqual(dry.returncode, 0, dry.stderr)
        self.assertEqual(dry.stdout.splitlines(), [
            "4wX-0wO-4bX-0bO", "4wX-0wO-3bX-1bO"])
        (self.data / "4wX-0wO-3bX-1bO.dtm").unlink()
        missing = self.run_driver()
        self.assertEqual(missing.returncode, 1)
        self.assertIn("missing requested DTMs", missing.stderr)
        self.assertFalse(self.state.exists())

    def test_family_order_is_canonical(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "verify_all.py"),
             "--directory", str(self.data), "--state-directory", str(self.state),
             "--dry-run", "--families", "6x2", "4x4", "--limit", "2"],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [
            "4wX-0wO-4bX-0bO", "4wX-0wO-3bX-1bO"])


if __name__ == "__main__":
    unittest.main()
