#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/fuzz.py.

    python -m unittest discover -s tools -p "test_fuzz.py" -v
"""

from __future__ import annotations

import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import fuzz

WORKFLOW = (fuzz.ROOT / ".github" / "workflows" / "fuzz.yml").read_text(encoding="utf-8")

# Time the job needs besides fuzzing: the toolchain, the dependencies, the cache, the build,
# and the start-up and corpus loading of each target. The longest run so far was about ten
# minutes of it.
JOB_OVERHEAD_MINUTES = 20


class HarnessDiscovery(unittest.TestCase):
    def test_every_harness_has_a_corpus_and_regressions_directory(self):
        sources = fuzz.harnesses()
        self.assertGreaterEqual(len(sources), 2)
        for source in sources:
            self.assertTrue((source.parent / "corpus").is_dir(), source)
            self.assertTrue((source.parent / "regressions").is_dir(), source)
            self.assertTrue(any((source.parent / "corpus").iterdir()), source)

    def test_every_harness_is_declared_in_cmake(self):
        declared = (fuzz.ROOT / "tests" / "fuzz" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        for source in fuzz.harnesses():
            self.assertIn(source.name, declared)


class CommandLines(unittest.TestCase):
    harness = Path("libs/x/fuzz/fuzz_thing.cpp")
    build = Path("build/linux-fuzz")
    work = Path(".cache/fuzz")

    def test_regress_replays_once_and_writes_nothing(self):
        line = fuzz.command(self.harness, self.build, "regress", self.work, 0)
        self.assertEqual(line[1], "-runs=0")
        self.assertNotIn(str(self.work / "fuzz_thing"), line)
        self.assertEqual(Path(line[0]).name, "fuzz_thing")

    def test_fuzzing_writes_new_inputs_to_the_working_corpus_first(self):
        line = fuzz.command(self.harness, self.build, "short", self.work, 60)
        self.assertIn("-max_total_time=60", line)
        first_directory = [word for word in line if not word.startswith("-")][1]
        self.assertEqual(Path(first_directory), self.work / "fuzz_thing")

    def test_the_times_are_the_standards(self):
        self.assertEqual(fuzz.SECONDS, {"short": 60, "nightly": 600})


class TimeBudget(unittest.TestCase):
    def test_without_a_budget_a_target_gets_the_time_of_the_mode(self):
        self.assertEqual(fuzz.seconds_per_target("nightly", 8, None), 600)
        self.assertEqual(fuzz.seconds_per_target("short", 8, None), 60)

    def test_a_budget_that_holds_every_target_changes_nothing(self):
        self.assertEqual(fuzz.seconds_per_target("nightly", 8, 90), 600)

    def test_a_target_added_shortens_the_others_and_not_the_run(self):
        for targets in range(1, 19):
            seconds = fuzz.seconds_per_target("nightly", targets, 90)
            self.assertLessEqual(seconds * targets, 90 * 60, targets)
        self.assertEqual(fuzz.seconds_per_target("nightly", 10, 90), 540)
        self.assertEqual(fuzz.seconds_per_target("nightly", 15, 90), 360)

    def test_too_many_targets_for_the_budget_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            fuzz.seconds_per_target("nightly", 19, 90)
        self.assertIn("split the targets", str(caught.exception))

    def test_replaying_has_no_time_to_share(self):
        self.assertEqual(fuzz.seconds_per_target("regress", 8, 1), 0)

    def test_main_refuses_a_budget_that_is_too_small_and_runs_nothing(self):
        with mock.patch.object(fuzz.subprocess, "run") as run:
            self.assertEqual(fuzz.main(["--mode", "nightly", "--budget-minutes", "1"]), 2)
        run.assert_not_called()

    def test_a_target_that_hangs_is_killed_and_the_others_still_run(self):
        sources = [Path("libs/a/fuzz/fuzz_a.cpp"), Path("libs/b/fuzz/fuzz_b.cpp")]
        calls = []

        def fake_run(line, check, timeout):
            calls.append((Path(line[0]).name, timeout))
            if Path(line[0]).name == "fuzz_a":
                raise subprocess.TimeoutExpired(line, timeout)
            return subprocess.CompletedProcess(line, 0)

        with tempfile.TemporaryDirectory() as work, mock.patch.object(
            fuzz, "harnesses", return_value=sources
        ), mock.patch.object(fuzz.subprocess, "run", side_effect=fake_run):
            status = fuzz.main(["--mode", "short", "--work-dir", work])
        self.assertEqual(status, 1)
        self.assertEqual(calls, [("fuzz_a", 60 + fuzz.GRACE_SECONDS), ("fuzz_b", 60 + fuzz.GRACE_SECONDS)])


class WorkflowLimit(unittest.TestCase):
    """fuzz.yml runs every target in one job, so the job limit has to follow the targets."""

    def number(self, pattern: str) -> int:
        found = re.search(pattern, WORKFLOW, re.MULTILINE)
        self.assertIsNotNone(found, pattern)
        return int(found.group(1))

    def test_the_job_limit_holds_the_budget_and_the_build(self):
        limit = self.number(r"^    timeout-minutes: (\d+)$")
        budget = self.number(r"^      FUZZ_BUDGET_MINUTES: (\d+)$")
        self.assertGreaterEqual(limit, budget + JOB_OVERHEAD_MINUTES)

    def test_the_run_passes_the_budget_to_the_script(self):
        self.assertIn('tools/fuzz.py --mode "$MODE" --budget-minutes "$FUZZ_BUDGET_MINUTES"', WORKFLOW)

    def test_every_target_fits_the_budget_at_night(self):
        budget = self.number(r"^      FUZZ_BUDGET_MINUTES: (\d+)$")
        targets = len(fuzz.harnesses())
        seconds = fuzz.seconds_per_target("nightly", targets, budget)
        self.assertLessEqual(seconds * targets, budget * 60)
        # The standard asks for 10 minutes a target; the budget should leave that, not just the floor.
        self.assertEqual(seconds, fuzz.SECONDS["nightly"], f"{targets} targets no longer get the full time")

    def test_a_timed_out_job_still_keeps_the_crash_and_reports(self):
        for name in ("Keep the crashing input", "Report the failure"):
            step = re.search(rf"- name: {name}\n        if: (.*)\n", WORKFLOW)
            self.assertIsNotNone(step, name)
            self.assertIn("failure()", step.group(1), name)
            self.assertIn("cancelled()", step.group(1), name)


if __name__ == "__main__":
    unittest.main()
