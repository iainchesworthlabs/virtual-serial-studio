#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of rules that every workflow in .github/workflows has to keep.

    python3 -m unittest discover -s tools/ci -p "test_workflows.py" -v

They read the workflow files as text, so they need no YAML library.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = sorted((ROOT / ".github" / "workflows").glob("*.yml"))
REPORTER = "icl-common-infra/actions/report-nightly-failure@"
SCHEDULED_CANCEL = "cancelled() && github.event_name == 'schedule'"


def steps_using(text: str, marker: str) -> list[list[str]]:
    """The lines of each step that has `marker` on a line of its own `uses:`."""
    lines = text.splitlines()
    found = []
    for index, line in enumerate(lines):
        if marker not in line:
            continue
        start = index
        while start > 0 and not re.match(r"^\s*- ", lines[start]):
            start -= 1
        end = index
        while end + 1 < len(lines) and not re.match(r"^\s*- ", lines[end + 1]) and lines[end + 1].strip():
            end += 1
        found.append(lines[start : end + 1])
    return found


def condition(step: list[str]) -> str:
    for line in step:
        match = re.match(r"^\s*(?:- )?if: (.*)$", line)
        if match:
            return match.group(1)
    return ""


class NightlyReporters(unittest.TestCase):
    """GitHub cancels a job that runs out of time, and failure() is false for a cancelled job."""

    def test_some_workflow_reports(self) -> None:
        self.assertTrue(any(steps_using(w.read_text(encoding="utf-8"), REPORTER) for w in WORKFLOWS))

    def test_every_reporter_runs_for_a_failure_and_for_a_timeout_of_a_scheduled_run(self) -> None:
        for workflow in WORKFLOWS:
            for step in steps_using(workflow.read_text(encoding="utf-8"), REPORTER):
                with self.subTest(workflow=workflow.name, step=step[0].strip()):
                    when = condition(step)
                    self.assertIn("failure()", when)
                    self.assertIn(SCHEDULED_CANCEL, when)
                    # A run started by hand and cancelled by hand must not open an issue.
                    self.assertNotIn("cancelled()", when.replace(SCHEDULED_CANCEL, ""))

    def test_the_check_notices_a_reporter_that_only_looks_for_failure(self) -> None:
        step = [
            "      - name: Report the failure",
            "        if: failure() && github.ref == 'refs/heads/develop'",
            f"        uses: iainchesworthlabs/{REPORTER}c412e6a # v0.1.19",
        ]
        text = "jobs:\n  a:\n    steps:\n      - run: x\n" + "\n".join(step) + "\n        with:\n          engine: e\n"
        found = steps_using(text, REPORTER)
        self.assertEqual(len(found), 1)
        self.assertNotIn(SCHEDULED_CANCEL, condition(found[0]))

    def test_the_check_notices_a_bare_cancelled(self) -> None:
        when = "${{ (failure() || cancelled()) && github.ref == 'refs/heads/develop' }}"
        self.assertNotIn(SCHEDULED_CANCEL, when)
        guarded = "${{ (failure() || (" + SCHEDULED_CANCEL + ")) && github.ref == 'refs/heads/develop' }}"
        self.assertNotIn("cancelled()", guarded.replace(SCHEDULED_CANCEL, ""))


if __name__ == "__main__":
    unittest.main()
