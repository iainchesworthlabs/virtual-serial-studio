#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for run_tool_tests.py.

    python3 -m unittest discover -s tools/ci -p "test_run_tool_tests.py" -v
"""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import run_tool_tests  # noqa: E402

RUNS = '''
import unittest

class Runs(unittest.TestCase):
    def test_a(self):
        pass

    def test_b(self):
        pass
'''

SKIPS = '''
import unittest

class Skips(unittest.TestCase):
    @unittest.skip("needs Windows PowerShell")
    def test_a(self):
        pass

    def test_b(self):
        pass
'''

FAILS = '''
import unittest

class Fails(unittest.TestCase):
    def test_a(self):
        self.fail("broken")
'''


class Runner(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.before = set(sys.modules)
        self.addCleanup(lambda: [sys.modules.pop(n) for n in set(sys.modules) - self.before if n.startswith("test_rtt_")])
        self.addCleanup(lambda: sys.path.remove(str(self.root)) if str(self.root) in sys.path else None)

    def run_with(self, files: dict[str, str], required: tuple[str, ...]) -> tuple[int, str]:
        for name, text in files.items():
            (self.root / f"{name}.py").write_text(text, encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            status = run_tool_tests.run(self.root, required, stream=io.StringIO())
        return status, out.getvalue() + err.getvalue()

    def test_tests_that_run_pass(self) -> None:
        status, text = self.run_with({"test_rtt_runs": RUNS}, ("test_rtt_runs",))
        self.assertEqual(status, 0, text)
        self.assertIn("2 tests ran, none skipped", text)

    def test_a_skip_in_a_required_module_fails(self) -> None:
        status, text = self.run_with({"test_rtt_skips": SKIPS}, ("test_rtt_skips",))
        self.assertEqual(status, 1)
        self.assertIn("1 of the 2 tests of test_rtt_skips were skipped", text)

    def test_a_skip_in_another_module_is_allowed(self) -> None:
        status, text = self.run_with({"test_rtt_skips": SKIPS, "test_rtt_runs": RUNS}, ("test_rtt_runs",))
        self.assertEqual(status, 0, text)

    def test_a_required_module_that_is_gone_fails(self) -> None:
        status, text = self.run_with({"test_rtt_runs": RUNS}, ("test_rtt_runs", "test_rtt_renamed"))
        self.assertEqual(status, 1)
        self.assertIn("test_rtt_renamed has no tests here", text)

    def test_a_failing_test_fails(self) -> None:
        status, _ = self.run_with({"test_rtt_fails": FAILS}, ())
        self.assertEqual(status, 1)

    def test_the_required_modules_are_the_ones_that_skip_without_powershell(self) -> None:
        for name in run_tool_tests.REQUIRED:
            self.assertTrue((ROOT / "tools" / f"{name}.py").is_file(), name)
            self.assertIn("skipUnless", (ROOT / "tools" / f"{name}.py").read_text(encoding="utf-8"), name)


if __name__ == "__main__":
    unittest.main()
