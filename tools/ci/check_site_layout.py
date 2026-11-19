#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the built site against tools/docs/site_layout.json: no page is lost, and each page is at the address the table gives it.

    python3 tools/ci/check_site_layout.py [--site build/docs] [--root .]

Without --site it builds the site with `mkdocs build --strict` into a temporary folder (it needs the packages of
requirements/docs.txt) and checks that. With --site it checks a folder that is already built.

site_layout.json lists every page of docs/, the address it has on https://iainchesworthlabs.com/virtual-serial-studio/ (`null`: the
page is published on GitHub only). Nothing was ever public at an older address, so the table holds new addresses only. This check
fails when

- a file in the table is not in docs/, or a page of docs/ (outside the folders that exclude_docs of mkdocs.yml names) is not in the table;
- a page that has an address is not in the built site, or its canonical address is not the one in the table, or two pages have one address;
- a page that has no address (GitHub only) is in the built site, or is not excluded by mkdocs.yml;
- a page of the navigation (`nav` of mkdocs.yml) has no address in the table;
- a page of the built site is at an address that the table does not give, or is still under user-guide/ (the guide is at guide/).

The same table is applied while the site is built (tools/docs/site_layout_hook.py).
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import site_address  # noqa: E402
import site_tools  # noqa: E402

# Folders of the built site that are not pages of docs/: Material's search index and the stylesheets and files the hooks add.
NOT_PAGES = ("assets/", "search/", "stylesheets/", "javascripts/")


def exclude_patterns(root: Path) -> list[str]:
    """The lines of `exclude_docs` in mkdocs.yml."""
    lines = (root / "mkdocs.yml").read_text(encoding="utf-8").splitlines()
    out: list[str] = []
    inside = False
    for line in lines:
        if line.startswith("exclude_docs:"):
            inside = True
        elif inside and line.startswith("  ") and line.strip():
            out.append(line.strip())
        elif inside and not line.startswith("  ") and line.strip() and not line.lstrip().startswith("#"):
            break
    return out


def nav_pages(root: Path) -> list[str]:
    """The Markdown files that `nav` of mkdocs.yml names."""
    lines = (root / "mkdocs.yml").read_text(encoding="utf-8").splitlines()
    out: list[str] = []
    inside = False
    for line in lines:
        if line.startswith("nav:"):
            inside = True
        elif inside and line.strip() and not line.startswith((" ", "#")):
            break
        elif inside:
            found = re.search(r":\s*([A-Za-z0-9_./-]+\.md)\s*$", line)
            if found:
                out.append(found.group(1))
    return out


def docs_pages(root: Path) -> list[str]:
    """Every Markdown file of docs/ that is not inside a folder that exclude_docs names."""
    folders = [p.strip("/") + "/" for p in exclude_patterns(root) if p.endswith("/")]
    out: list[str] = []
    for path in sorted((root / "docs").rglob("*.md")):
        rel = path.relative_to(root / "docs").as_posix()
        if not any(rel.startswith(folder) for folder in folders):
            out.append(rel)
    return out


def table_problems(root: Path) -> list[str]:
    """The table against docs/ and mkdocs.yml, which needs no built site."""
    problems: list[str] = []
    entries = json.loads((root / "tools" / "docs" / "site_layout.json").read_text(encoding="utf-8"))["pages"]
    excluded = {p.strip("/") for p in exclude_patterns(root) if not p.endswith("/")}
    by_src = {entry["src"]: entry for entry in entries}
    seen: dict[str, str] = {}
    for entry in entries:
        src, new = entry["src"], entry["new"]
        if not (root / "docs" / src).is_file():
            problems.append(f"site_layout.json: docs/{src} is not in docs/")
        if new is None:
            if src not in excluded:
                problems.append(f"site_layout.json: docs/{src} has no address (GitHub only) but exclude_docs of mkdocs.yml does not name it")
            continue
        if src in excluded:
            problems.append(f"site_layout.json: docs/{src} has the address {new!r} but exclude_docs of mkdocs.yml names it")
        if new in seen:
            problems.append(f"site_layout.json: {new or '(home)'} is the address of docs/{seen[new]} and of docs/{src}")
        seen[new] = src
    for src in docs_pages(root):
        if src not in by_src:
            problems.append(f"docs/{src}: is not in site_layout.json; give it an address, or null if it is published on GitHub only")
    for src in nav_pages(root):
        entry = by_src.get(src)
        if entry is None or entry["new"] is None:
            problems.append(f"mkdocs.yml: the navigation names docs/{src}, which has no address in site_layout.json")
    return problems


def check(site: Path, root: Path) -> list[str]:
    """The table, and the built site against it."""
    problems = table_problems(root)
    site_url = site_address.site_url(root)
    entries = json.loads((root / "tools" / "docs" / "site_layout.json").read_text(encoding="utf-8"))["pages"]
    addresses = {entry["new"]: entry["src"] for entry in entries if entry["new"] is not None}
    for new, src in addresses.items():
        built = site / new / "index.html"
        if not built.is_file():
            problems.append(f"{new or '(home)'}: is not in the built site (site_layout.json gives it to docs/{src})")
            continue
        canonical = site_tools.parse(built).canonical
        if canonical != site_url + new:
            problems.append(f"{new or '(home)'}: its canonical address is {canonical!r}, not {site_url + new!r}")
    for built in sorted(site.rglob("index.html")):
        folder = built.parent.relative_to(site).as_posix()
        address = "" if folder == "." else folder + "/"
        if address.startswith(NOT_PAGES):
            continue
        if address not in addresses:
            problems.append(f"{address or '(home)'}: a page of the built site is at an address that site_layout.json does not give to any page")
    if (site / "user-guide").exists():
        problems.append("user-guide/: the guide is at guide/; a page of the built site is still under user-guide/")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--site", type=Path, help="a site that is already built")
    args = parser.parse_args(argv)
    tmp = None
    site = args.site
    if site is None:
        if not site_tools.material_installed():
            print("check_site_layout: mkdocs-material is not installed (pip install --require-hashes -r requirements/docs.txt): cannot build the site", file=sys.stderr)
            return 2
        tmp = tempfile.mkdtemp(prefix="vss-site-")
        site = Path(tmp)
        if site_tools.build(args.root, site) != 0:
            shutil.rmtree(tmp, ignore_errors=True)
            return 1
    try:
        problems = check(site, args.root)
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
    for problem in problems:
        print(f"::error::{problem}")
    if problems:
        return 1
    print("Site layout: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
