#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/release_preflight.py.

    python -m unittest discover -s tools -p "test_release_preflight.py" -v
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

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


class Runs(unittest.TestCase):
    sample = {"conclusion": "success", "event": "schedule", "headBranch": "develop", "createdAt": "2026-11-08T01:02:03Z"}

    def test_success_is_met(self):
        self.assertTrue(tool.judge_run("CI", self.sample).met)

    def test_failure_is_unmet(self):
        self.assertFalse(tool.judge_run("CI", {**self.sample, "conclusion": "failure"}).met)

    def test_no_run_is_unmet(self):
        self.assertFalse(tool.judge_run("CI", None).met)


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
