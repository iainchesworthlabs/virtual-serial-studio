#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of tools/docs/publish_site.py and of the workflow that uses it.

    python -m unittest discover -s tools/ci -p "test_publish_site.py" -v

They make small repositories in a temporary folder with `git init`, so they need git and nothing else: no network, no GitHub and no
YAML library. `site` is the branch that is published and `develop` the branch that the site files come from.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "docs"))

import publish_site  # noqa: E402

WORKFLOW = ROOT / ".github" / "workflows" / "publish-site.yml"
DOCS_WORKFLOW = ROOT / ".github" / "workflows" / "docs.yml"
RELEASE_WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"
PATTERNS = publish_site.load_patterns()


class Matching(unittest.TestCase):
    def test_a_double_star_matches_across_folders(self) -> None:
        self.assertTrue(publish_site.matches("docs/index.md", ["docs/**"]))
        self.assertTrue(publish_site.matches("docs/user-guide/a/b.md", ["docs/**"]))

    def test_a_single_star_stays_in_one_folder_name(self) -> None:
        self.assertTrue(publish_site.matches("tools/ci/site_tools.py", ["tools/ci/site_*.py"]))
        self.assertFalse(publish_site.matches("tools/ci/sub/site_tools.py", ["tools/ci/site_*.py"]))
        self.assertFalse(publish_site.matches("tools/ci/site_tools.txt", ["tools/ci/site_*.py"]))

    def test_an_exact_name_matches_only_itself(self) -> None:
        self.assertTrue(publish_site.matches("mkdocs.yml", ["mkdocs.yml"]))
        self.assertFalse(publish_site.matches("docs/mkdocs.yml", ["mkdocs.yml"]))
        self.assertFalse(publish_site.matches("mkdocs.yml.bak", ["mkdocs.yml"]))
        self.assertFalse(publish_site.matches("mkdocsXyml", ["mkdocs.yml"]))

    def test_a_folder_with_a_similar_name_does_not_match(self) -> None:
        self.assertFalse(publish_site.matches("docs-site/x", ["docs/**"]))
        self.assertFalse(publish_site.matches("docs", ["docs/**"]))
        self.assertFalse(publish_site.matches("mydocs/x.md", ["docs/**"]))

    def test_a_double_star_between_folders_may_be_empty(self) -> None:
        self.assertTrue(publish_site.matches("a/b.md", ["a/**/b.md"]))
        self.assertTrue(publish_site.matches("a/x/y/b.md", ["a/**/b.md"]))
        self.assertFalse(publish_site.matches("a/x/c.md", ["a/**/b.md"]))

    def test_a_regular_expression_character_in_a_pattern_is_plain(self) -> None:
        self.assertTrue(publish_site.matches("a+b.md", ["a+b.md"]))
        self.assertFalse(publish_site.matches("aab.md", ["a+b.md"]))

    def test_any_one_pattern_is_enough(self) -> None:
        self.assertTrue(publish_site.matches("mkdocs.yml", ["docs/**", "mkdocs.yml"]))
        self.assertFalse(publish_site.matches("mkdocs.yml", []))

    def test_the_real_list(self) -> None:
        for path in (
            "docs/index.md",
            "docs/assets/guide.css",
            "mkdocs.yml",
            "requirements/docs.in",
            "requirements/docs.txt",
            "tools/docs/publish_site.py",
            "tools/docs/site_paths.json",
            ".github/actions/docs-site/action.yml",
            "tools/ci/site_tools.py",
            "tools/ci/check_site_pages.py",
            "tools/ci/check_release_data.py",
            "tools/ci/check_claims.py",
            "tools/ci/test_check_claims.py",
            "tools/ci/test_site_checks.py",
            "tools/ci/test_publish_site.py",
        ):
            with self.subTest(path=path):
                self.assertTrue(publish_site.matches(path, PATTERNS))
        for path in (
            "CHANGELOG.md",
            "README.md",
            "CMakeLists.txt",
            "libs/vss_core/x.cpp",
            "apps/vss_gui/main.cpp",
            ".github/workflows/docs.yml",
            ".github/workflows/publish-site.yml",
            "tools/ci/precheck.py",
            "tools/ci/plan_legs.py",
            "tools/check_docs.py",
            "docs-site/x",
            "changelog.d/a.fixed.md",
        ):
            with self.subTest(path=path):
                self.assertFalse(publish_site.matches(path, PATTERNS))

    def test_the_list_is_well_formed(self) -> None:
        data = json.loads((ROOT / "tools" / "docs" / "site_paths.json").read_text(encoding="utf-8"))
        self.assertIn("comment", data)
        self.assertEqual(len(PATTERNS), len(set(PATTERNS)))
        for pattern in PATTERNS:
            self.assertFalse(pattern.startswith("/"), pattern)
            self.assertNotIn("\\", pattern)


@contextlib.contextmanager
def isolated_git():
    """Nothing of the machine's own git configuration may reach the tests: no identity, no signing, no hooks."""
    with unittest.mock.patch.dict(
        os.environ, {"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1", "GIT_TERMINAL_PROMPT": "0"}
    ):
        for name in ("GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL", "GIT_INDEX_FILE"):
            os.environ.pop(name, None)
        yield


class RepositoryCase(unittest.TestCase):
    """A temporary repository with a `site` branch at the last release and a `develop` branch ahead of it."""

    template: Path | None = None
    scratch: tempfile.TemporaryDirectory[str] | None = None

    @classmethod
    def setUpClass(cls) -> None:
        """Make the starting repository once; each test works on a copy of it."""
        cls.scratch = tempfile.TemporaryDirectory()
        cls.template = Path(cls.scratch.name) / "template"
        cls.template.mkdir()
        with isolated_git():
            case = cls("setUp")
            case.repo = cls.template
            case.git("init", "-q", "-b", "develop")
            case.write("docs/index.md", "home\n")
            case.write("docs/gone.md", "to be removed\n")
            case.write("docs/guide/a.md", "guide a\n")
            case.write("mkdocs.yml", "site_name: x\n")
            case.write("libs/x.cpp", "int x;\n")
            case.write("CHANGELOG.md", "release notes\n")
            case.write("README.md", "readme\n")
            case.commit("The release")
            case.git("branch", "site")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.scratch is not None:
            cls.scratch.cleanup()

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name) / "repo"
        assert self.template is not None
        shutil.copytree(self.template, self.repo)
        isolation = isolated_git()
        isolation.__enter__()
        self.addCleanup(isolation.__exit__, None, None, None)

    def git(self, *args: str) -> str:
        done = subprocess.run(
            ["git", "-C", str(self.repo), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
             "-c", "commit.gpgsign=false", "-c", "core.autocrlf=false", *args],
            capture_output=True, text=True, check=False,
        )
        if done.returncode != 0:
            self.fail(f"git {' '.join(args)}: {done.stderr}")
        return done.stdout.strip()

    def write(self, name: str, text: str) -> None:
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")

    def remove(self, name: str) -> None:
        (self.repo / name).unlink()

    def commit(self, message: str) -> str:
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def run_tool(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = publish_site.main(["--repo", str(self.repo), *args])
        return code, out.getvalue()

    def build(self, message: str = "Publish the site") -> tuple[int, str]:
        return self.run_tool("build", "--site", "site", "--source", "develop", "--message", message)

    def file_at(self, ref: str, name: str) -> str | None:
        done = subprocess.run(["git", "-C", str(self.repo), "show", f"{ref}:{name}"], capture_output=True, text=True, check=False)
        return done.stdout if done.returncode == 0 else None


class Plan(RepositoryCase):
    def test_a_site_change_is_listed_and_a_code_change_is_not(self) -> None:
        self.write("docs/index.md", "home, changed\n")
        self.write("docs/new.md", "new page\n")
        self.remove("docs/gone.md")
        self.write("libs/x.cpp", "int y;\n")
        self.write("CHANGELOG.md", "newer notes\n")
        self.commit("Site and code")
        code, out = self.run_tool("plan", "--site", "site", "--source", "develop")
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines(), ["D docs/gone.md", "M docs/index.md", "A docs/new.md", "3 files to publish"])
        self.assertNotIn("libs/x.cpp", out)
        self.assertNotIn("CHANGELOG.md", out)

    def test_nothing_to_publish_exits_with_zero(self) -> None:
        self.write("libs/x.cpp", "int y;\n")
        self.commit("Code only")
        code, out = self.run_tool("plan", "--site", "site", "--source", "develop")
        self.assertEqual(code, 0)
        self.assertEqual(out.strip(), "nothing to publish")

    def test_one_file_says_file_and_not_files(self) -> None:
        self.write("mkdocs.yml", "site_name: y\n")
        self.commit("One")
        _code, out = self.run_tool("plan", "--site", "site", "--source", "develop")
        self.assertEqual(out.splitlines(), ["M mkdocs.yml", "1 file to publish"])

    def test_a_ref_that_does_not_exist_is_an_error(self) -> None:
        code, out = self.run_tool("plan", "--site", "origin/site", "--source", "develop")
        self.assertEqual(code, 2)
        self.assertIn("origin/site", out)


class Build(RepositoryCase):
    def test_a_site_change_is_published_and_a_code_change_is_not_copied(self) -> None:
        self.write("docs/index.md", "home, changed\n")
        self.write("docs/guide/b.md", "guide b\n")
        self.write("libs/x.cpp", "int y;\n")
        self.write("CHANGELOG.md", "newer notes\n")
        self.commit("Site and code")
        site_before = self.git("rev-parse", "site")
        code, out = self.build("Publish the site from tests")
        self.assertEqual(code, 0, out)
        commit = out.strip()
        self.assertRegex(commit, r"^[0-9a-f]{40}$")
        self.assertEqual(self.git("rev-parse", f"{commit}^"), site_before)
        self.assertEqual(self.git("rev-list", "--parents", "-n", "1", commit).split(), [commit, site_before])
        self.assertEqual(self.git("log", "-1", "--format=%s", commit), "Publish the site from tests")
        self.assertEqual(self.file_at(commit, "docs/index.md"), "home, changed\n")
        self.assertEqual(self.file_at(commit, "docs/guide/b.md"), "guide b\n")
        self.assertEqual(self.file_at(commit, "libs/x.cpp"), "int x;\n")
        self.assertEqual(self.file_at(commit, "CHANGELOG.md"), "release notes\n")
        changed = self.git("diff", "--name-only", site_before, commit).splitlines()
        self.assertEqual(changed, ["docs/guide/b.md", "docs/index.md"])

    def test_a_deleted_site_file_is_deleted_from_site(self) -> None:
        self.remove("docs/gone.md")
        self.commit("Remove a page")
        code, out = self.build()
        self.assertEqual(code, 0, out)
        commit = out.strip()
        self.assertIsNone(self.file_at(commit, "docs/gone.md"))
        self.assertEqual(self.file_at(commit, "docs/index.md"), "home\n")

    def test_a_file_outside_the_site_paths_that_differs_is_left_alone(self) -> None:
        # The site branch holds a file that develop has deleted and a file that develop has changed, both outside the site paths.
        self.git("switch", "-q", "site")
        self.write("extra/only-on-site.txt", "kept\n")
        self.commit("Site only file")
        self.git("switch", "-q", "develop")
        self.remove("README.md")
        self.write("libs/x.cpp", "int z;\n")
        self.write("docs/index.md", "home, changed\n")
        self.commit("Develop moves on")
        code, out = self.build()
        self.assertEqual(code, 0, out)
        commit = out.strip()
        self.assertEqual(self.file_at(commit, "extra/only-on-site.txt"), "kept\n")
        self.assertEqual(self.file_at(commit, "README.md"), "readme\n")
        self.assertEqual(self.file_at(commit, "libs/x.cpp"), "int x;\n")
        self.assertEqual(self.file_at(commit, "docs/index.md"), "home, changed\n")

    def test_nothing_to_publish_exits_with_three_and_makes_no_commit(self) -> None:
        self.write("libs/x.cpp", "int y;\n")
        self.commit("Code only")
        before = self.git("rev-list", "--all", "--count")
        code, out = self.build()
        self.assertEqual(code, publish_site.NOTHING)
        self.assertEqual(code, 3)
        self.assertEqual(out.strip(), "nothing to publish")
        self.assertEqual(self.git("rev-list", "--all", "--count"), before)

    def test_the_branches_the_index_and_the_working_tree_are_not_touched(self) -> None:
        self.write("docs/index.md", "home, changed\n")
        self.commit("Site change")
        self.write("docs/index.md", "home, edited but not committed\n")
        heads = self.git("for-each-ref", "--format=%(refname) %(objectname)", "refs/heads")
        head = self.git("symbolic-ref", "HEAD")
        status = self.git("status", "--porcelain")
        code, out = self.build()
        self.assertEqual(code, 0, out)
        self.assertEqual(self.git("for-each-ref", "--format=%(refname) %(objectname)", "refs/heads"), heads)
        self.assertEqual(self.git("symbolic-ref", "HEAD"), head)
        self.assertEqual(self.git("status", "--porcelain"), status)
        self.assertEqual((self.repo / "docs/index.md").read_text(encoding="utf-8"), "home, edited but not committed\n")

    def test_the_commit_has_an_author_without_any_configuration(self) -> None:
        self.write("mkdocs.yml", "site_name: y\n")
        self.commit("Site change")
        code, out = self.build()
        self.assertEqual(code, 0, out)
        self.assertEqual(self.git("log", "-1", "--format=%an <%ae>", out.strip()), f"{publish_site.FALLBACK_NAME} <{publish_site.FALLBACK_EMAIL}>")

    def test_the_identity_in_the_environment_is_used(self) -> None:
        self.write("mkdocs.yml", "site_name: y\n")
        self.commit("Site change")
        env = {"GIT_AUTHOR_NAME": "Bot", "GIT_AUTHOR_EMAIL": "bot@example.invalid",
               "GIT_COMMITTER_NAME": "Bot", "GIT_COMMITTER_EMAIL": "bot@example.invalid"}
        with unittest.mock.patch.dict(os.environ, env):
            code, out = self.build()
        self.assertEqual(code, 0, out)
        self.assertEqual(self.git("log", "-1", "--format=%an <%ae>", out.strip()), "Bot <bot@example.invalid>")

    def test_a_file_that_becomes_a_folder_and_a_name_with_a_space(self) -> None:
        # On site, docs/guide is a file. On develop it is a folder, as it was at the release, and it gains a file with a space.
        self.git("switch", "-q", "site")
        self.git("rm", "-q", "-r", "docs/guide")
        self.write("docs/guide", "now a file\n")
        self.commit("On site, guide is a file")
        self.git("switch", "-q", "develop")
        self.write("docs/guide/c d.md", "spaced\n")
        self.commit("On develop, guide is a folder")
        code, out = self.build()
        self.assertEqual(code, 0, out)
        commit = out.strip()
        self.assertEqual(self.file_at(commit, "docs/guide/c d.md"), "spaced\n")
        self.assertEqual(self.file_at(commit, "docs/guide/a.md"), "guide a\n")
        self.assertEqual(self.git("ls-tree", "-d", "--name-only", commit, "docs/guide"), "docs/guide")

    def test_the_commit_is_not_made_when_the_tree_would_be_the_same(self) -> None:
        # develop changes a site file and changes it back.
        self.write("docs/index.md", "other\n")
        self.commit("Change")
        self.write("docs/index.md", "home\n")
        self.commit("Change back")
        code, _out = self.build()
        self.assertEqual(code, 3)


class Guard(RepositoryCase):
    def push(self, before: str, after: str, *extra: str) -> tuple[int, str]:
        return self.run_tool("guard", "--before", before, "--after", after, *extra)

    def test_a_site_only_push_is_accepted(self) -> None:
        before = self.git("rev-parse", "develop")
        self.write("docs/index.md", "home, changed\n")
        self.write("mkdocs.yml", "site_name: y\n")
        after = self.commit("Site only")
        code, out = self.push(before, after)
        self.assertEqual(code, 0, out)
        self.assertIn("Allowed", out)

    def test_a_push_that_changes_code_is_refused(self) -> None:
        before = self.git("rev-parse", "develop")
        self.write("docs/index.md", "home, changed\n")
        self.write("libs/x.cpp", "int y;\n")
        after = self.commit("Site and code")
        code, out = self.push(before, after)
        self.assertEqual(code, 1, out)
        self.assertIn("libs/x.cpp", out)
        self.assertNotIn("docs/index.md", out)

    def test_a_deleted_file_outside_the_site_paths_is_refused(self) -> None:
        before = self.git("rev-parse", "develop")
        self.remove("CHANGELOG.md")
        after = self.commit("Remove the notes")
        code, out = self.push(before, after)
        self.assertEqual(code, 1, out)
        self.assertIn("CHANGELOG.md", out)

    def test_a_renamed_file_is_judged_by_both_names(self) -> None:
        before = self.git("rev-parse", "develop")
        self.git("mv", "libs/x.cpp", "docs/x.cpp")
        after = self.commit("Move code into docs")
        code, out = self.push(before, after)
        self.assertEqual(code, 1, out)
        self.assertIn("libs/x.cpp", out)

    def test_a_push_with_the_tree_of_a_release_tag_is_accepted(self) -> None:
        before = self.git("rev-parse", "site")
        self.write("libs/x.cpp", "int y;\n")
        self.write("CHANGELOG.md", "v0.1.0 notes\n")
        tagged = self.commit("The next release")
        self.git("tag", "v0.1.0")
        # A fast-forward to the tag.
        code, out = self.push(before, tagged)
        self.assertEqual(code, 0, out)
        self.assertIn("v0.1.0", out)
        # A merge commit that takes the tag's tree, as the release workflow makes it after a site-only update.
        tree = self.git("rev-parse", f"{tagged}^{{tree}}")
        merge = self.git("commit-tree", tree, "-p", before, "-p", tagged, "-m", "Publish the site from v0.1.0")
        code, out = self.push(before, merge)
        self.assertEqual(code, 0, out)
        self.assertIn("v0.1.0", out)

    def test_a_tree_that_is_not_a_tag_tree_is_refused_even_with_a_tag_elsewhere(self) -> None:
        before = self.git("rev-parse", "site")
        self.write("libs/x.cpp", "int y;\n")
        tagged = self.commit("The next release")
        self.git("tag", "v0.1.0")
        self.write("libs/x.cpp", "int z;\n")
        after = self.commit("More code")
        self.assertEqual(self.push(before, tagged)[0], 0)
        code, out = self.push(before, after)
        self.assertEqual(code, 1, out)

    def test_a_tag_that_does_not_match_the_glob_does_not_count(self) -> None:
        before = self.git("rev-parse", "site")
        self.write("libs/x.cpp", "int y;\n")
        after = self.commit("Code")
        self.git("tag", "nightly-1")
        code, _out = self.push(before, after)
        self.assertEqual(code, 1)
        code, _out = self.push(before, after, "--tags-glob", "nightly-*")
        self.assertEqual(code, 0)

    def test_a_new_branch_is_allowed(self) -> None:
        after = self.git("rev-parse", "develop")
        code, out = self.push("0" * 40, after)
        self.assertEqual(code, 0, out)
        self.assertIn("new", out)

    def test_a_before_that_is_not_in_the_repository_is_an_error(self) -> None:
        code, out = self.push("1" * 40, self.git("rev-parse", "develop"))
        self.assertEqual(code, 2, out)


def lines_under(text: str, key: str, after: str = "") -> list[str]:
    """The quoted items of the YAML list that follows the first `key:` line after the first line that holds `after`."""
    lines = text.splitlines()
    start = next((n for n, line in enumerate(lines) if after in line), 0)
    for index in range(start, len(lines)):
        if lines[index].strip() == f"{key}:":
            indent = len(lines[index]) - len(lines[index].lstrip())
            items = []
            for line in lines[index + 1 :]:
                match = re.match(r"^(\s*)- (.*?)\s*$", line)
                if not match or len(match.group(1)) <= indent:
                    break
                items.append(match.group(2).strip("\"'"))
            return items
    return []


class Workflow(unittest.TestCase):
    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8")

    def test_the_trigger_paths_are_the_site_paths(self) -> None:
        self.assertEqual(lines_under(self.text, "paths", "  push:"), PATTERNS)

    def test_the_trigger_is_develop_and_a_run_by_hand(self) -> None:
        self.assertRegex(self.text, r"(?m)^  push:\n    branches: \[develop\]\n")
        self.assertRegex(self.text, r"(?m)^  workflow_dispatch:")
        triggers = self.text.split("\njobs:")[0].split("\non:")[1]
        self.assertNotIn("pull_request", re.sub(r"(?m)^\s*#.*$", "", triggers))

    def test_the_workflow_asks_for_nothing_by_default_and_never_forces(self) -> None:
        self.assertRegex(self.text, r"(?m)^permissions: \{\}$")
        self.assertRegex(self.text, r"(?m)^concurrency:\n  group: publish-site\n  cancel-in-progress: false$")
        self.assertNotRegex(self.text, r"--force|--force-with-lease|push -f| -f \S*refs")
        self.assertIn("environment: site-release", self.text)
        self.assertIn("::add-mask::", self.text)

    def test_the_release_job_needs_the_plan_and_is_reached_only_when_something_changed(self) -> None:
        release = self.text[self.text.index("\n  release:") :]
        self.assertIn("needs: plan", release)
        self.assertIn("needs.plan.outputs.changed == 'true'", release)

    def test_every_action_is_pinned_to_a_full_sha_with_a_version(self) -> None:
        for line in self.text.splitlines():
            match = re.search(r"\buses:\s*(\S+)", line)
            if match:
                self.assertRegex(line, r"@[0-9a-f]{40} # v\d+\.\d+\.\d+$", line)

    def test_the_pins_are_the_ones_the_release_workflow_uses(self) -> None:
        release = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        for line in self.text.splitlines():
            match = re.search(r"uses:\s*(actions/(?:checkout|create-github-app-token)@\S+ # \S+)", line)
            if match:
                self.assertIn(match.group(1), release + DOCS_WORKFLOW.read_text(encoding="utf-8"))


class OtherWorkflows(unittest.TestCase):
    def test_the_deploy_waits_for_the_guard_and_runs_only_for_a_push_to_site(self) -> None:
        text = DOCS_WORKFLOW.read_text(encoding="utf-8")
        guard = text[text.index("\n  guard:") : text.index("\n  build:")]
        self.assertIn("publish_site.py guard", guard)
        self.assertIn("fetch-depth: 0", guard)
        self.assertIn("refs/heads/site", guard)
        deploy = text[text.index("\n  deploy:") : text.index("\n  deploy-failed:")]
        self.assertIn("needs: [build, guard]", deploy)
        self.assertIn("needs.guard.result == 'success'", deploy)
        self.assertIn("needs.build.result == 'success'", deploy)
        self.assertIn("github.ref == 'refs/heads/site'", deploy)

    def test_the_release_job_moves_the_site_branch_without_force(self) -> None:
        text = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        job = text[text.index("\n  site:") :]
        self.assertRegex(job, r"(?m)^    permissions: \{\}$")
        self.assertIn("force=false", job)
        self.assertNotIn("force=true", job)
        self.assertIn("/compare/", job)
        self.assertIn("Publish the site from", job)

    def test_the_new_test_is_in_the_docs_class_of_the_plan(self) -> None:
        plan = json.loads((ROOT / ".github" / "ci" / "plan.json").read_text(encoding="utf-8"))
        for path in ("tools/docs/publish_site.py", "tools/docs/site_paths.json", "tools/ci/test_publish_site.py"):
            with self.subTest(path=path):
                self.assertTrue(publish_site.matches(path, plan["docs"]))


if __name__ == "__main__":
    unittest.main()
