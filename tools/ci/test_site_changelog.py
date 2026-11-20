#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Self-test of the What's new hook: tools/docs/site_changelog_hook.py.

The hook turns the released sections of CHANGELOG.md into Markdown where a page writes <!-- changelog:releases -->. The tests give it
small changelogs made here and the real CHANGELOG.md of the repository, and need neither MkDocs nor a built site. Run it with
`python -I tools/ci/test_site_changelog.py`, or through `unittest discover -s tools/ci -p "test_*.py"`.
"""

import re
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tools" / "docs"))

import site_changelog_hook as hook  # noqa: E402

REPO_URL = "https://github.com/example/site"

CHANGELOG = """# Changelog

All notable changes are documented here.

## [Unreleased]

### Added

- **Not released.** A change that waits for the next release.

## [1.1.0] - 2026-12-01

### Added

- **A new thing.** It is described in [the guide](docs/user-guide/index.md#ports) and in [the folder](docs/images/) and on [GitHub](https://github.com/x/y).

### Fixed

- **A fault.** It no longer happens.

## [1.0.0] - 2026-11-20

### Added

- **The first thing.** See [a heading on this page](#nowhere).

[Unreleased]: https://github.com/example/site/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/example/site/releases/tag/v1.1.0
[1.0.0]: https://github.com/example/site/releases/tag/v1.0.0
"""

ONLY_UNRELEASED = """# Changelog

## [Unreleased]

[Unreleased]: https://github.com/example/site/compare/v0.1.0...HEAD
"""


def headings(markdown: str) -> list[tuple[int, str]]:
    return [(len(found.group(1)), found.group(2)) for found in re.finditer(r"^(#{1,6}) (.*)$", markdown, flags=re.M)]


class Render(unittest.TestCase):
    def test_each_release_is_a_heading_with_its_date_an_address_and_a_link_to_the_release(self) -> None:
        out = hook.render(CHANGELOG, REPO_URL)
        self.assertIn("## 1.1.0, 1 December 2026 { #v1-1-0 }", out)
        self.assertIn("## 1.0.0, 20 November 2026 { #v1-0-0 }", out)
        self.assertIn(f"[This release on GitHub]({REPO_URL}/releases/tag/v1.1.0)", out)
        self.assertLess(out.index("1.1.0, 1 December"), out.index("1.0.0, 20 November"))

    def test_the_outline_is_one_level_deep_below_the_pages_h1(self) -> None:
        self.assertEqual(headings(hook.render(CHANGELOG, REPO_URL)),
                         [(2, "1.1.0, 1 December 2026 { #v1-1-0 }"), (3, "Added"), (3, "Fixed"), (2, "1.0.0, 20 November 2026 { #v1-0-0 }"), (3, "Added")])

    def test_headings_can_start_lower_and_never_reach_above_the_version(self) -> None:
        out = hook.render(CHANGELOG, REPO_URL, first_level=3)
        self.assertEqual([level for level, _ in headings(out)], [3, 4, 4, 3, 4])
        odd = "## [1.0.0] - 2026-11-20\n\n# Odd\n\n## Odder\n\n- text\n"
        self.assertEqual([level for level, _ in headings(hook.render(odd, REPO_URL))], [2, 3, 3])
        self.assertEqual(max(level for level, _ in headings(hook.render(CHANGELOG, REPO_URL, first_level=6))), 6)

    def test_the_unreleased_section_and_the_link_definitions_are_left_out(self) -> None:
        out = hook.render(CHANGELOG, REPO_URL)
        self.assertNotIn("Not released", out)
        self.assertNotIn("Unreleased", out)
        self.assertNotIn("compare/", out)
        self.assertNotIn("]: ", out)

    def test_a_relative_link_becomes_a_link_to_the_file_on_github_and_an_absolute_one_stays(self) -> None:
        out = hook.render(CHANGELOG, REPO_URL)
        self.assertIn("(https://github.com/example/site/blob/develop/docs/user-guide/index.md#ports)", out)
        self.assertIn("(https://github.com/example/site/tree/develop/docs/images)", out)
        self.assertIn("(https://github.com/x/y)", out)
        self.assertIn("(#nowhere)", out)

    def test_a_fenced_block_is_not_touched(self) -> None:
        text = "## [1.0.0] - 2026-11-20\n\n### Added\n\n```\n# not a heading\n[a](docs/x.md)\n```\n"
        out = hook.render(text, REPO_URL)
        self.assertIn("# not a heading\n[a](docs/x.md)", out)
        self.assertEqual(headings(out.replace("# not a heading", "")), [(2, "1.0.0, 20 November 2026 { #v1-0-0 }"), (3, "Added")])

    def test_a_version_with_no_date_and_one_with_no_text(self) -> None:
        out = hook.render("## [2.0.0]\n\n- x\n\n## [1.0.0] - 2026-01-02\n", REPO_URL)
        self.assertIn("## 2.0.0 { #v2-0-0 }", out)
        self.assertNotIn("1.0.0", out)

    def test_with_no_release_it_says_so_in_one_sentence(self) -> None:
        out = hook.render(ONLY_UNRELEASED, REPO_URL)
        self.assertEqual(out, hook.NO_RELEASE)
        self.assertEqual(headings(out), [])

    def test_with_no_release_and_entries_waiting_it_shows_the_unreleased_heading(self) -> None:
        text = "## [Unreleased]\n\n### Added\n\n- **Waiting.** A change.\n"
        out = hook.render(text, REPO_URL)
        self.assertTrue(out.startswith(hook.NO_RELEASE))
        self.assertEqual(headings(out), [(2, "Unreleased { #unreleased }"), (3, "Added")])
        self.assertIn("**Waiting.**", out)

    def test_a_heading_that_is_not_a_version_stops_the_build(self) -> None:
        with self.assertRaises(hook.ChangelogError):
            hook.render("## [Unreleased]\n\n## [next] - soon\n", REPO_URL)


class Fill(unittest.TestCase):
    def test_only_the_marker_is_replaced(self) -> None:
        page = "# What's new\n\nSome text.\n\n<!-- changelog:releases -->\n\nMore text.\n"
        out = hook.fill(page, CHANGELOG, REPO_URL)
        self.assertTrue(out.startswith("# What's new\n\nSome text.\n\n## 1.1.0"))
        self.assertTrue(out.endswith("\n\nMore text.\n"))
        self.assertNotIn("changelog:releases", out)

    def test_a_page_without_the_marker_is_unchanged(self) -> None:
        page = "# Other\n\n<!-- releases:version -->\n\n## [1.0.0] - 2026-11-20\n"
        self.assertEqual(hook.fill(page, CHANGELOG, REPO_URL), page)

    def test_an_unknown_marker_stops_the_build(self) -> None:
        for marker in ("<!-- changelog:release -->", "<!-- changelog: -->", "<!--changelog:unreleased-->"):
            with self.assertRaises(hook.ChangelogError, msg=marker):
                hook.fill(f"x\n{marker}\n", CHANGELOG, REPO_URL)

    def test_the_hook_reads_the_changelog_beside_docs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "docs").mkdir()
            (root / "CHANGELOG.md").write_text(CHANGELOG, encoding="utf-8")
            config = {"docs_dir": str(root / "docs"), "repo_url": REPO_URL + "/"}
            out = hook.on_page_markdown("<!-- changelog:releases -->\n", None, config, None)
            self.assertIn("## 1.1.0, 1 December 2026", out)
            self.assertEqual(hook.on_page_markdown("no marker\n", None, config, None), "no marker\n")


class Repository(unittest.TestCase):
    """The changelog and the page of this repository."""

    def test_the_changelog_of_the_repository_renders(self) -> None:
        text = (REPO / hook.CHANGELOG).read_text(encoding="utf-8")
        out = hook.render(text, "https://github.com/iainchesworthlabs/virtual-serial-studio")
        released = [version for version, _, body in hook.sections(text)[1] if "".join(body).strip()]
        if released:
            # Each released section is a heading of the page's second level, and nothing starts above it.
            self.assertEqual([level for level, _ in headings(out)].count(2), len(released))
        else:
            self.assertTrue(out.startswith(hook.NO_RELEASE))
        self.assertGreaterEqual(min((level for level, _ in headings(out)), default=2), 2)
        self.assertNotIn("](docs/", out)

    def test_the_page_has_one_h1_and_the_marker(self) -> None:
        page = (REPO / "docs" / "whats-new.md").read_text(encoding="utf-8").split("\n---\n", 1)[1]
        self.assertEqual(len(re.findall(r"^# ", page, flags=re.M)), 1)
        self.assertEqual(len(hook.MARKER_RE.findall(page)), 1)
        self.assertEqual(hook.MARKER_RE.search(page).group(1), "releases")


if __name__ == "__main__":
    unittest.main()
