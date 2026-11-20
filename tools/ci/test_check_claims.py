#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of tools/ci/check_claims.py (found by `unittest discover -s tools/ci`, so the changelog job runs them)."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_claims  # noqa: E402


class Claims(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "docs" / "plan").mkdir(parents=True)
        (self.root / "docs" / "gui-design").mkdir(parents=True)
        (self.root / "tools" / "docs").mkdir(parents=True)
        self.config: dict = {"exceptions": []}
        self.write_config()
        self.page("docs/index.md", "A plain page about serial ports.\n")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write_config(self) -> None:
        (self.root / check_claims.EXCEPTIONS).write_text(json.dumps(self.config), encoding="utf-8")

    def page(self, name: str, text: str) -> None:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def problems(self) -> str:
        return "\n".join(check_claims.check(self.root)[0])

    def exception(self, file: str = "docs/support.md", phrase: str = "no guaranteed response time", approved_by: str = "") -> None:
        self.config["exceptions"] = [{"file": file, "phrase": phrase, "reason": "x", "approved_by": approved_by}]
        self.write_config()

    def test_a_plain_tree_passes(self) -> None:
        self.assertEqual(check_claims.check(self.root), ([], [], []))

    def test_the_repository_passes(self) -> None:
        problems, _, unused = check_claims.check(check_claims.ROOT)
        self.assertEqual(problems, [])
        self.assertEqual(unused, [])

    def test_each_word_is_found_in_any_case_and_with_its_line(self) -> None:
        for word in ("certified", "Qualified", "COMPLIANT", "approved", "guaranteed"):
            self.page("docs/index.md", f"One line.\nThe program is {word}.\n")
            self.assertIn(f"docs/index.md:2: says '{word}'", self.problems())

    def test_a_longer_word_is_not_a_claim(self) -> None:
        self.page("docs/index.md", "A certificate authority, a qualification, an unapproved draft and a guarantee.\n")
        self.assertEqual(self.problems(), "")

    def test_the_plans_and_the_design_bundle_are_not_scanned_and_the_root_files_are(self) -> None:
        self.page("docs/plan/x.md", "The program is not qualified.\n")
        self.page("docs/gui-design/x.md", "The mockup is approved.\n")
        self.assertEqual(self.problems(), "")
        for name in ("README.md", "SECURITY.md", "CONTRIBUTING.md", "CHANGELOG.md"):
            self.page(name, "It is approved.\n")
            self.assertIn(f"{name}:1", self.problems())
            (self.root / name).unlink()
        self.page("changelog.d/x.added.md", "Guaranteed.\n")
        self.assertIn("changelog.d/x.added.md:1", self.problems())

    def test_an_exception_covers_its_phrase_in_its_file_across_a_line_break(self) -> None:
        self.page("docs/support.md", "There is no guaranteed\nresponse time.\n")
        self.assertIn("docs/support.md:1", self.problems())
        self.exception()
        self.assertEqual(self.problems(), "")

    def test_an_exception_does_not_cover_another_file_or_another_use(self) -> None:
        self.exception()
        self.page("docs/support.md", "No guaranteed response time. The program is guaranteed.\n")
        self.page("docs/faq.md", "No guaranteed response time.\n")
        text = self.problems()
        self.assertIn("docs/support.md:1: says 'guaranteed'", text)
        self.assertIn("docs/faq.md:1", text)

    def test_an_exception_that_matches_nothing_is_reported(self) -> None:
        self.exception(file="docs/gone.md")
        self.assertEqual(len(check_claims.check(self.root)[2]), 1)
        self.assertEqual(check_claims.main(["--root", str(self.root)]), 1)

    def test_an_exception_waits_for_approval_and_strict_fails_until_it_has_it(self) -> None:
        self.page("docs/support.md", "No guaranteed response time.\n")
        self.exception()
        self.assertEqual(len(check_claims.check(self.root)[1]), 1)
        self.assertEqual(check_claims.main(["--root", str(self.root)]), 0)
        self.assertEqual(check_claims.main(["--root", str(self.root), "--strict"]), 1)
        self.exception(approved_by="the owner")
        self.assertEqual(check_claims.check(self.root)[1], [])
        self.assertEqual(check_claims.main(["--root", str(self.root), "--strict"]), 0)

    def test_a_blank_approval_does_not_count(self) -> None:
        self.page("docs/support.md", "No guaranteed response time.\n")
        self.exception(approved_by="   ")
        self.assertEqual(check_claims.main(["--root", str(self.root), "--strict"]), 1)

    def test_no_page_may_name_a_standard_and_no_page_is_excused(self) -> None:
        for standard in ("DO-178C", "DO-330", "ISO 26262", "IEC 62304"):
            self.page("docs/features.md", f"It follows {standard}.\n")
            self.assertIn(f"docs/features.md:1: names {standard}", self.problems())
        self.config["standards_named_in"] = ["docs/features.md"]
        self.write_config()
        self.assertIn("docs/features.md:1: names", self.problems())

    def test_a_missing_exceptions_file_is_an_error_not_a_pass(self) -> None:
        (self.root / check_claims.EXCEPTIONS).unlink()
        self.assertEqual(check_claims.main(["--root", str(self.root)]), 2)


if __name__ == "__main__":
    unittest.main()
