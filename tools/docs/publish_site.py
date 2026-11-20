#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Publish the product site between releases (docs/ci.md, "Publishing the site between releases").

    python tools/docs/publish_site.py plan  --site <ref> --source <ref>
    python tools/docs/publish_site.py build --site <ref> --source <ref> --message <text>
    python tools/docs/publish_site.py guard --before <sha> --after <sha> [--tags-glob 'v*']

The live site is built from the branch `site`. Its files that are not site paths (the application code, the build files, the
workflows, CHANGELOG.md) stay at the last release; the files that are site paths are listed in tools/docs/site_paths.json.

plan   Prints what publishing would change: the files under the site paths that differ between the tree of `--site` and the tree of
       `--source`, one per line as `A path` (added), `M path` (changed) or `D path` (deleted), and then the count. It says
       "nothing to publish" when there are none. Exit status 0.
build  Makes a commit whose tree is the tree of `--site` with every file under a site path replaced by its version in `--source`
       (a file that `--site` has under a site path and `--source` has not is deleted), and whose only parent is `--site`. A file that
       is not under a site path is never touched. It prints the commit id and leaves it for the caller to push. When the new tree is
       the tree of `--site` it prints "nothing to publish" and exits with status 3. It uses git plumbing and a temporary index, so
       the working tree, the index and the branches of the repository are not touched.
guard  For a push to `site`: lists the files that changed between `--before` and `--after`. Exit status 0 when every one is under a
       site path, or when the tree of `--after` is the tree of a tag that matches `--tags-glob` (a release move: a fast-forward to the
       tag, or a merge commit that takes the tag's tree). Otherwise it prints the files that are not allowed and exits with status 1.
       A `--before` of all zeros (the branch is new) is allowed, and the output says so.

Exit status 2 is a usage or git error. The standard library is all it needs; the only program it runs is git.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from collections.abc import Iterable, Sequence
from pathlib import Path

HERE = Path(__file__).resolve().parent
SITE_PATHS = HERE / "site_paths.json"
ZEROS = "0" * 40
NOTHING = 3
DEFAULT_TAGS_GLOB = "v*"
# Used only when neither the environment nor the git configuration names an identity (a runner has none).
FALLBACK_NAME = "github-actions[bot]"
FALLBACK_EMAIL = "41898282+github-actions[bot]@users.noreply.github.com"


class GitError(RuntimeError):
    """A git command failed or its answer was not what the tool expects."""


def load_patterns(path: Path = SITE_PATHS) -> list[str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    patterns = data["paths"]
    if not isinstance(patterns, list) or not all(isinstance(p, str) and p for p in patterns):
        raise ValueError(f"{path}: `paths` has to be a list of non-empty strings")
    return patterns


def pattern_regex(pattern: str) -> re.Pattern[str]:
    """`*` is any run of characters but `/`, `?` is one such character, `**` is any run at all (`**/` may also be empty)."""
    out: list[str] = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out) + r"\Z", re.DOTALL)


def matches(path: str, patterns: Iterable[str]) -> bool:
    """Whether the repository-relative path (with `/`) matches one of the patterns."""
    return any(pattern_regex(pattern).match(path) for pattern in patterns)


def git_bytes(repo: Path, *args: str, env: dict[str, str] | None = None, stdin: bytes | None = None) -> bytes:
    command = ["git", "-C", str(repo), *args]
    try:
        done = subprocess.run(command, input=stdin, capture_output=True, check=False, env=env)
    except OSError as error:
        raise GitError(f"cannot run git: {error}") from error
    if done.returncode != 0:
        detail = done.stderr.decode("utf-8", "replace").strip()
        raise GitError(f"git {' '.join(args[:2])} failed ({done.returncode}): {detail}")
    return done.stdout


def git(repo: Path, *args: str, env: dict[str, str] | None = None, stdin: bytes | None = None) -> str:
    return git_bytes(repo, *args, env=env, stdin=stdin).decode("utf-8", "surrogateescape").strip()


def rev(repo: Path, ref: str) -> str:
    """The commit id of a ref, or an error that names the ref."""
    try:
        return git(repo, "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}")
    except GitError as error:
        raise GitError(f"{ref} is not a commit of this repository") from error


def tree_of(repo: Path, ref: str) -> str:
    return git(repo, "rev-parse", "--verify", f"{ref}^{{tree}}")


# path -> (mode, object id)
Entries = dict[str, tuple[str, str]]


def list_tree(repo: Path, ref: str) -> Entries:
    """Every file of the tree of `ref`, with its mode and object id (a symbolic link and a submodule are entries too)."""
    out = git_bytes(repo, "ls-tree", "-r", "-z", "--full-tree", ref)
    entries: Entries = {}
    for record in out.split(b"\0"):
        if not record:
            continue
        meta, _, raw_path = record.partition(b"\t")
        mode, _kind, sha = meta.decode("ascii").split(" ")
        entries[raw_path.decode("utf-8", "surrogateescape")] = (mode, sha)
    return entries


def under_site(entries: Entries, patterns: Sequence[str]) -> Entries:
    return {path: value for path, value in entries.items() if matches(path, patterns)}


def changes(site: Entries, source: Entries) -> list[tuple[str, str]]:
    """The (letter, path) of every difference, sorted by path. Both arguments hold site paths only."""
    found: list[tuple[str, str]] = []
    for path in sorted(site.keys() | source.keys()):
        if path not in site:
            found.append(("A", path))
        elif path not in source:
            found.append(("D", path))
        elif site[path] != source[path]:
            found.append(("M", path))
    return found


def compare(repo: Path, site_ref: str, source_ref: str, patterns: Sequence[str]) -> tuple[Entries, Entries, list[tuple[str, str]]]:
    site = under_site(list_tree(repo, site_ref), patterns)
    source = under_site(list_tree(repo, source_ref), patterns)
    return site, source, changes(site, source)


def cmd_plan(args: argparse.Namespace, patterns: Sequence[str]) -> int:
    repo = Path(args.repo)
    rev(repo, args.site)
    rev(repo, args.source)
    _site, _source, found = compare(repo, args.site, args.source, patterns)
    for letter, path in found:
        print(f"{letter} {path}")
    if not found:
        print("nothing to publish")
    else:
        print(f"{len(found)} file{'s' if len(found) != 1 else ''} to publish")
    return 0


def identity_options(repo: Path) -> list[str]:
    """`-c` options that give commit-tree a name and an email only when there is none to use."""
    options: list[str] = []
    for key, variables, fallback in (
        ("user.name", ("GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME"), FALLBACK_NAME),
        ("user.email", ("GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"), FALLBACK_EMAIL),
    ):
        if any(os.environ.get(variable) for variable in variables):
            continue
        try:
            configured = git(repo, "config", "--get", key)
        except GitError:
            configured = ""
        if not configured:
            options += ["-c", f"{key}={fallback}"]
    return options


def cmd_build(args: argparse.Namespace, patterns: Sequence[str]) -> int:
    repo = Path(args.repo)
    site_commit = rev(repo, args.site)
    rev(repo, args.source)
    site, source, found = compare(repo, args.site, args.source, patterns)
    if not found:
        print("nothing to publish")
        return NOTHING

    # Removals first, so that a file that becomes a folder (or the other way round) does not collide in the index.
    lines: list[bytes] = []
    for letter, path in found:
        if letter == "D":
            lines.append(f"0 {ZEROS}\t{path}".encode("utf-8", "surrogateescape") + b"\0")
    for letter, path in found:
        if letter != "D":
            mode, sha = source[path]
            lines.append(f"{mode} {sha}\t{path}".encode("utf-8", "surrogateescape") + b"\0")

    with tempfile.TemporaryDirectory(prefix="publish-site-") as scratch:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(scratch) / "index"))
        git(repo, "read-tree", site_commit, env=env)
        git(repo, "update-index", "-z", "--index-info", env=env, stdin=b"".join(lines))
        tree = git(repo, "write-tree", env=env)

    if tree == tree_of(repo, site_commit):
        print("nothing to publish")
        return NOTHING
    commit = git(
        repo, *identity_options(repo), "-c", "commit.gpgsign=false",
        "commit-tree", tree, "-p", site_commit, "-m", args.message,
    )
    print(commit)
    return 0


def release_trees(repo: Path, tags_glob: str) -> dict[str, str]:
    """tree id -> the first tag with that tree, for every tag that matches the glob."""
    trees: dict[str, str] = {}
    for tag in git(repo, "tag", "--list", tags_glob).splitlines():
        try:
            trees.setdefault(tree_of(repo, f"refs/tags/{tag}"), tag)
        except GitError:
            continue
    return trees


def cmd_guard(args: argparse.Namespace, patterns: Sequence[str]) -> int:
    repo = Path(args.repo)
    if re.fullmatch(r"0{7,64}", args.before):
        print("The branch is new, so there is no earlier state to compare with. Allowed.")
        return 0
    rev(repo, args.before)
    rev(repo, args.after)
    out = git_bytes(repo, "diff-tree", "-r", "--no-renames", "--name-only", "-z", args.before, args.after)
    changed = [p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p]
    refused = [path for path in changed if not matches(path, patterns)]
    print(f"{len(changed)} file{'s' if len(changed) != 1 else ''} changed between {args.before[:12]} and {args.after[:12]}.")
    if not refused:
        print("Every one is a site path. Allowed.")
        return 0
    tag = release_trees(repo, args.tags_glob).get(tree_of(repo, args.after))
    if tag is not None:
        print(f"The tree is the tree of the tag {tag}, so this is the move of a release. Allowed.")
        return 0
    print(f"{len(refused)} of them {'is' if len(refused) == 1 else 'are'} not a site path, and the tree is not the tree of a "
          f"tag that matches {args.tags_glob}. Refused:")
    for path in refused:
        print(f"  {path}")
    print("Only site files may reach `site` between releases (docs/ci.md, \"Publishing the site between releases\").")
    return 1


def parser() -> argparse.ArgumentParser:
    top = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    top.add_argument("--repo", default=".", help="the repository (default: the current directory)")
    top.add_argument("--paths", type=Path, default=SITE_PATHS, help="the list of site paths (default: tools/docs/site_paths.json)")
    sub = top.add_subparsers(dest="command", required=True)
    for name in ("plan", "build"):
        one = sub.add_parser(name)
        one.add_argument("--site", required=True, help="the ref of the published site, such as origin/site")
        one.add_argument("--source", required=True, help="the ref that the site files come from, such as origin/develop")
        if name == "build":
            one.add_argument("--message", required=True, help="the commit message")
    guard = sub.add_parser("guard")
    guard.add_argument("--before", required=True)
    guard.add_argument("--after", required=True)
    guard.add_argument("--tags-glob", default=DEFAULT_TAGS_GLOB)
    return top


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        patterns = load_patterns(args.paths)
        handler = {"plan": cmd_plan, "build": cmd_build, "guard": cmd_guard}[args.command]
        return handler(args, patterns)
    except (GitError, ValueError, KeyError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
