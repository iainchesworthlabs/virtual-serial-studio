#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail when the documentation site's address is written down in more than one way.

    python3 tools/ci/check_site_address.py [--root .]

`site_url` in mkdocs.yml is the one place that holds the address, and it is fixed: https://iainchesworthlabs.com/virtual-serial-studio/
(the company site serves this program under /virtual-serial-studio/ and no other path). The pages of docs/ write `{{ site_url }}`
where they need it and a hook (tools/docs/site_address_hook.py) fills it in at build time.

The program itself has a second address compiled in, https://virtual-serial-studio.com (the update feed, the documentation and the
downloads page; apps/vss_gui/src/updates/update_types.hpp and apps/vss_gui/src/support/app_identity.cpp). It is not the site's address,
and the program is not changed to move it, so the second domain forwards to the site (docs/plan/site.md). This check fails when

- `site_url` is not the fixed address;
- a page of docs/ writes the site's host or the old address (the GitHub Pages site) out, other than the pages that describe the
  address (docs/ci.md and the plans);
- a file writes the compiled-in host out and is not one of COMPILED_IN: the program's source and tests, its translations, the
  changelog, a sample feed, and the pages of docs/ that describe what the program does with that address;
- README.md names the old address, or links to the site's host at another address than `site_url`.

A move to another address is then: change `site_url` and FIXED_ADDRESS in tools/ci/site_address.py, fix what this check names, and
change the deploy job's prefix (owner: the company site's apps.ts; docs/ci.md, "The site address").
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import site_address  # noqa: E402

OLD_HOST = "iainchesworthlabs.github.io"
COMPILED_IN_HOST = "virtual-serial-studio.com"

# The pages that describe the site's address and its history may name every host.
PAGES_THAT_MAY_NAME_THE_HOSTS = ("docs/ci.md", "docs/plan/")

# Where the compiled-in host may be written: a path that equals an entry or starts with one that ends in "/".
COMPILED_IN = (
    "apps/vss_gui/src/",  # updates/update_types.hpp and support/app_identity.cpp hold it; the dialogs' text and the tests repeat it
    "apps/vss_gui/translations/",
    "libs/vss_update/fuzz/corpus/",  # a sample feed
    "changelog.d/",
    "CHANGELOG.md",
    "docs/ci.md",
    "docs/plan/",
    "docs/gui-design/",  # the screen specifications copy the dialogs' text
    "docs/updates.md",  # the design of the update check, which asks that address
    "docs/user-guide.md",  # says what the update check asks, and where
    "docs/releasing.md",  # the feed is published at that address
    "docs/editions.md",  # the proposal for the editions page
    "tools/ci/check_site_address.py",
    "tools/ci/site_address.py",
    "tools/ci/test_site_checks.py",
)

# What is looked in for the compiled-in host. Build output, vendored code and binary files are not.
ROOTS = ("apps", "libs", "tests", "tools", "packaging", "docs", "changelog.d", ".github", "adapters", "sdk", "perf", "drivers")
TOP_LEVEL = ("README.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md", "AGENTS.md", "mkdocs.yml")
SKIP_DIRS = {"build", "third_party", "__pycache__", "node_modules", ".git", ".icl-infra"}
TEXT_SUFFIXES = {".md", ".cpp", ".hpp", ".h", ".c", ".ts", ".json", ".py", ".yml", ".yaml", ".html", ".css", ".txt", ".cmake", ".qml",
                 ".ps1", ".sh", ".nsi", ".nsh", ".in", ".toml", ".xml", ".svg", ".js", ".mjs", ".cfg", ".ini", ".rc", ".inf"}
MAX_BYTES = 2_000_000


def allowed(rel: str, entries: tuple[str, ...]) -> bool:
    return any(rel == entry or (entry.endswith("/") and rel.startswith(entry)) for entry in entries)


def candidate_files(root: Path):  # noqa: ANN201
    for name in TOP_LEVEL:
        if (root / name).is_file():
            yield root / name
    for top in ROOTS:
        base = root / top
        if not base.is_dir():
            continue
        stack = [base]
        while stack:
            folder = stack.pop()
            for entry in sorted(folder.iterdir()):
                if entry.is_dir():
                    if entry.name not in SKIP_DIRS:
                        stack.append(entry)
                elif entry.suffix.lower() in TEXT_SUFFIXES and entry.stat().st_size <= MAX_BYTES:
                    yield entry


def check(root: Path) -> list[str]:
    problems: list[str] = []
    site = site_address.site_url(root)
    host = site_address.host(root)
    if site != site_address.FIXED_ADDRESS:
        problems.append(f"mkdocs.yml: site_url is {site!r}; the address of this site is {site_address.FIXED_ADDRESS!r}")
    for page in sorted((root / "docs").rglob("*.md")):
        rel = page.relative_to(root).as_posix()
        if allowed(rel, PAGES_THAT_MAY_NAME_THE_HOSTS):
            continue
        text = page.read_text(encoding="utf-8")
        if host in text:
            problems.append(f"{rel}: writes the site's host out; write {{{{ site_url }}}} so one edit moves the site")
        if OLD_HOST in text:
            problems.append(f"{rel}: names the old address ({OLD_HOST}); the site is at {site}")
    for path in candidate_files(root):
        rel = path.relative_to(root).as_posix()
        if allowed(rel, COMPILED_IN):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if COMPILED_IN_HOST in text:
            problems.append(f"{rel}: writes the compiled-in address ({COMPILED_IN_HOST}) out. Only the files in COMPILED_IN of "
                            "tools/ci/check_site_address.py may; the site's own address is {{ site_url }}")
    readme = root / "README.md"
    if readme.is_file():
        text = readme.read_text(encoding="utf-8")
        if OLD_HOST in text:
            problems.append(f"README.md: names the old address ({OLD_HOST}); the site is at {site}")
        for url in re.findall(rf"https://{re.escape(host)}[^\s\"')>]*", text):
            if not url.startswith(site):
                problems.append(f"README.md: {url} is not under {site}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=site_address.ROOT)
    args = parser.parse_args(argv)
    try:
        problems = check(args.root)
    except (OSError, ValueError) as error:
        print(f"check_site_address: cannot check: {error}", file=sys.stderr)
        return 2
    for problem in problems:
        print(f"::error::{problem}")
    if problems:
        return 1
    print("Site address: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
