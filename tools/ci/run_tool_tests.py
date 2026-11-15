#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the unit tests of tools/ and fail when a test that must run here was skipped.

    python tools/ci/run_tool_tests.py                      # tools/, the modules in REQUIRED
    python tools/ci/run_tool_tests.py --require test_x ...

The tests of the Windows installer's PATH and stop scripts (tools/test_edit_path.py and
tools/test_stop_studio.py) skip themselves without Windows PowerShell, which every Linux
runner lacks. ci.yml runs this on a Windows runner, where they have to run, so a skip there
means the check no longer checks anything and fails the job. It also fails when a required
module is missing or has no tests, so renaming a test file cannot switch the check off.
"""

from __future__ import annotations

import argparse
import sys
import unittest
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Modules that skip without Windows PowerShell and must not skip on the runner that runs this.
REQUIRED = ("test_edit_path", "test_stop_studio")


def modules_of(suite: unittest.TestSuite) -> Counter[str]:
    counts: Counter[str] = Counter()
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            counts.update(modules_of(test))
        else:
            counts[type(test).__module__] += 1
    return counts


def run(start: Path, required: tuple[str, ...], pattern: str = "test_*.py", stream=None) -> int:
    suite = unittest.defaultTestLoader.discover(str(start), pattern=pattern, top_level_dir=str(start))
    found = modules_of(suite)
    result = unittest.TextTestRunner(stream=stream or sys.stderr, verbosity=1).run(suite)
    skipped = Counter(type(test).__module__ for test, _ in result.skipped)

    failed = False
    for name in required:
        if not found[name]:
            print(f"error: {name} has no tests here, so the check of it does not run", file=sys.stderr)
            failed = True
        elif skipped[name]:
            print(f"error: {skipped[name]} of the {found[name]} tests of {name} were skipped", file=sys.stderr)
            failed = True
        else:
            print(f"{name}: {found[name]} tests ran, none skipped")
    return 0 if result.wasSuccessful() and not failed else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--start", type=Path, default=ROOT / "tools", help="the directory to discover tests in")
    parser.add_argument("--require", nargs="*", default=list(REQUIRED), help="test modules that must run without a skip")
    args = parser.parse_args(argv)
    return run(args.start, tuple(args.require))


if __name__ == "__main__":
    sys.exit(main())
