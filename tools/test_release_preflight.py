#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/release_preflight.py.

    python -m unittest discover -s tools -p "test_release_preflight.py" -v
"""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

import release_preflight as tool


def alert(path: str, rule: str = "cpp/demo") -> dict:
    return {"rule": {"id": rule}, "most_recent_instance": {"location": {"path": path}}}


class Alerts(unittest.TestCase):
    def test_none_open_is_met(self):
        self.assertTrue(tool.judge_alerts([]).met)

    def test_third_party_paths_do_not_count(self):
        result = tool.judge_alerts([alert(".cache/boost-root/a.hpp"), alert("build/x/moc.cpp")])
        self.assertTrue(result.met)
        self.assertIn("2 more", result.detail)

    def test_a_first_party_alert_holds_the_release(self):
        result = tool.judge_alerts([alert("libs/vss_core/src/hub.cpp", "cpp/a"), alert("build/y.cpp")])
        self.assertFalse(result.met)
        self.assertIn("1 open", result.detail)
        self.assertIn("cpp/a x1", result.detail)


class Issues(unittest.TestCase):
    def test_open_issues_are_listed(self):
        result = tool.judge_issues([{"number": 44}, {"number": 60}])
        self.assertFalse(result.met)
        self.assertIn("#44, #60", result.detail)

    def test_no_issues_is_met(self):
        self.assertTrue(tool.judge_issues([]).met)


SHA = "a" * 40
OLDER = "b" * 40
NOW = datetime(2026, 11, 15, 9, 0, tzinfo=timezone.utc)


class Runs(unittest.TestCase):
    sample = {
        "conclusion": "success",
        "event": "schedule",
        "headBranch": "develop",
        "headSha": SHA,
        "createdAt": "2026-11-15T01:02:03Z",
    }

    def test_success_on_the_commit_is_met(self):
        self.assertTrue(tool.judge_run("CI", self.sample, tool.COMMIT, SHA).met)

    def test_failure_is_unmet(self):
        self.assertFalse(tool.judge_run("CI", {**self.sample, "conclusion": "failure"}, tool.COMMIT, SHA).met)

    def test_a_timed_out_run_is_unmet(self):
        self.assertFalse(tool.judge_run("Fuzz", {**self.sample, "conclusion": "cancelled"}, tool.RECENT, SHA, NOW).met)

    def test_no_run_is_unmet(self):
        result = tool.judge_run("CI", None, tool.COMMIT, SHA)
        self.assertFalse(result.met)
        self.assertIn("aaaaaaa", result.detail)

    def test_a_green_run_of_another_commit_does_not_release_this_one(self):
        result = tool.judge_run("Release dry run", {**self.sample, "headSha": OLDER}, tool.COMMIT, SHA)
        self.assertFalse(result.met)
        self.assertIn("older commit", result.detail)

    def test_an_unreadable_commit_is_unknown_not_met(self):
        self.assertIsNone(tool.judge_run("CI", self.sample, tool.COMMIT, None).met)

    def test_a_recent_run_of_the_commit_is_met(self):
        self.assertTrue(tool.judge_run("Nightly", self.sample, tool.RECENT, SHA, NOW).met)

    def test_a_recent_run_of_an_ancestor_is_met_when_github_says_so(self):
        older = {**self.sample, "headSha": OLDER}
        self.assertTrue(tool.judge_run("Nightly", older, tool.RECENT, SHA, NOW, relation="ahead").met)

    def test_a_recent_run_of_a_commit_outside_the_history_is_unmet(self):
        older = {**self.sample, "headSha": OLDER}
        for relation in ("behind", "diverged", None):
            result = tool.judge_run("Nightly", older, tool.RECENT, SHA, NOW, relation=relation)
            self.assertFalse(result.met, relation)
            self.assertIn("not in the history", result.detail)

    def test_a_run_older_than_the_limit_is_unmet(self):
        old = {**self.sample, "createdAt": "2026-11-12T01:02:03Z"}
        result = tool.judge_run("Nightly", old, tool.RECENT, SHA, NOW)
        self.assertFalse(result.met)
        self.assertIn("older than 2 days", result.detail)

    def test_a_run_at_the_edge_of_the_limit_is_met(self):
        edge = {**self.sample, "createdAt": "2026-11-13T09:00:00Z"}
        self.assertTrue(tool.judge_run("Nightly", edge, tool.RECENT, SHA, NOW).met)


class RunQueries(unittest.TestCase):
    """What is asked of GitHub, with `gh` replaced."""

    def fake(self, pages: dict[str, object]):
        asked: list[str] = []

        def run(command, **_):
            path = command[2]
            asked.append(path)
            for key, value in pages.items():
                if key in path:
                    text = value if isinstance(value, str) else json.dumps(value)
                    return subprocess.CompletedProcess(command, 0, stdout=text, stderr="")
            return subprocess.CompletedProcess(command, 1, stdout="", stderr="not found")

        return asked, mock.patch.object(tool.subprocess, "run", side_effect=run)

    def run_of(self, event: str, sha: str = SHA) -> dict:
        return {
            "conclusion": "success",
            "event": event,
            "head_branch": "develop",
            "head_sha": sha,
            "created_at": "2026-11-15T01:02:03Z",
        }

    def test_every_workflow_is_read_on_develop_including_docs(self):
        for workflow in tool.WORKFLOWS:
            query = tool.runs_query("o/r", workflow.file, tool.BRANCH)
            self.assertIn("branch=develop", query, workflow.file)
            self.assertIn("status=completed", query)

    def test_a_commit_rule_asks_for_the_runs_of_that_commit(self):
        self.assertIn(f"head_sha={SHA}", tool.runs_query("o/r", "ci.yml", "develop", SHA))
        self.assertNotIn("head_sha", tool.runs_query("o/r", "nightly.yml", "develop"))

    def test_the_fuzz_workflow_is_read_and_only_its_scheduled_runs_count(self):
        fuzz = next(w for w in tool.WORKFLOWS if w.file == "fuzz.yml")
        self.assertEqual(fuzz.events, ("schedule",))
        pages = {"fuzz.yml": {"workflow_runs": [self.run_of("push", OLDER), self.run_of("schedule")]}}
        asked, patched = self.fake(pages)
        with patched:
            run = tool.latest_run("o/r", "fuzz.yml", "develop", None, fuzz.events)
        self.assertEqual(run["event"], "schedule")
        self.assertEqual(run["headSha"], SHA)

    def test_no_run_of_the_right_event_is_none_and_an_unreadable_page_is_false(self):
        asked, patched = self.fake({"fuzz.yml": {"workflow_runs": [self.run_of("push")]}})
        with patched:
            self.assertIsNone(tool.latest_run("o/r", "fuzz.yml", "develop", None, ("schedule",)))
            self.assertIs(tool.latest_run("o/r", "other.yml", "develop"), False)

    def test_collect_reads_every_workflow_against_the_tip_of_develop(self):
        pages = {"commits/develop": SHA + "\n", "/runs?": {"workflow_runs": [self.run_of("schedule")]}}
        asked, patched = self.fake(pages)
        with patched:
            results = tool.collect("o/r", None)
        names = [r.name for r in results]
        for workflow in tool.WORKFLOWS:
            self.assertIn(f"{workflow.label} on develop", names)
        runs = [q for q in asked if "/runs?" in q]
        self.assertEqual(len(runs), len(tool.WORKFLOWS))
        self.assertTrue(all("branch=develop" in q for q in runs))
        for workflow, query in zip(tool.WORKFLOWS, runs):
            self.assertIn(f"/{workflow.file}/", query)
            self.assertEqual(f"head_sha={SHA}" in query, workflow.rule == tool.COMMIT, workflow.file)

    def test_a_workflow_that_gh_cannot_read_is_unknown(self):
        asked, patched = self.fake({"commits/develop": SHA})
        with patched:
            results = tool.collect("o/r", None)
        reads = [r for r in results if r.name.endswith("on develop")]
        self.assertEqual(len(reads), len(tool.WORKFLOWS))
        self.assertTrue(all(r.met is None for r in reads))


class WorkflowList(unittest.TestCase):
    def test_every_workflow_listed_exists(self):
        for workflow in tool.WORKFLOWS:
            self.assertTrue((tool.ROOT / ".github" / "workflows" / workflow.file).is_file(), workflow.file)

    def test_the_nightly_fuzz_job_is_part_of_the_release_conditions(self):
        self.assertIn("fuzz.yml", [w.file for w in tool.WORKFLOWS])

    def test_every_scheduled_analysis_that_opens_an_issue_is_read(self):
        listed = {w.file for w in tool.WORKFLOWS}
        for file in ("nightly.yml", "static-analysis.yml", "codeql.yml", "msvc-analysis.yml", "platform-matrix.yml"):
            self.assertIn(file, listed)


class Secrets(unittest.TestCase):
    def test_unreadable_is_unknown(self):
        self.assertIsNone(tool.judge_secrets(None, "false").met)

    def test_the_test_certificate_is_wanted_only_for_test_signing(self):
        names = set(tool.GPG_SECRETS)
        self.assertTrue(tool.judge_secrets(names, "false").met)
        result = tool.judge_secrets(names, "test")
        self.assertFalse(result.met)
        self.assertIn("WIN_TESTSIGN_PFX_BASE64", result.detail)

    def test_a_missing_gpg_secret_is_named(self):
        result = tool.judge_secrets(set(tool.GPG_SECRETS) - {"REPO_GPG_PASSPHRASE"}, None)
        self.assertFalse(result.met)
        self.assertIn("REPO_GPG_PASSPHRASE", result.detail)


class Environment(unittest.TestCase):
    def test_a_reviewer_is_required(self):
        reviewed = {"protection_rules": [{"type": "required_reviewers", "reviewers": [{"reviewer": {}}]}]}
        self.assertTrue(tool.judge_environment(reviewed).met)
        self.assertFalse(tool.judge_environment({"protection_rules": []}).met)
        self.assertFalse(tool.judge_environment(None).met)


class Files(unittest.TestCase):
    def test_missing_files_are_reported(self):
        root = Path(tempfile.mkdtemp())
        self.assertEqual([r.met for r in tool.judge_files(root)], [False, False])
        (root / "SECURITY.md").write_text("x", encoding="utf-8")
        self.assertEqual([r.met for r in tool.judge_files(root)], [False, True])


if __name__ == "__main__":
    unittest.main()
