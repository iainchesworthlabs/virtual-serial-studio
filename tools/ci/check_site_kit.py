#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hold the company kit copy and the palette of the /virtual-serial-studio/ pages to the company's rules.

    python3 tools/ci/check_site_kit.py [--root .]

The /virtual-serial-studio/ pages use the shell of iainchesworthlabs.com (its stylesheet, header and footer). Nothing is fetched at build or run
time: tools/docs/site-kit/v1/ is a pinned copy of the company's kit (its README says how it is published). This check fails when

- a file of the copy differs from the hash recorded in the copy's own SHA256SUMS.txt (line endings are not compared), or a file
  that SHA256SUMS.txt lists is missing, or a file is there that it does not list;
- docs/theme.css, the palette of the pages (the 8 colour roles, light and dark), does not pass the company's checker,
  kit/check-palette.mjs (shape and WCAG 2.2 AA contrast against the shell's own neutrals). The checker is Node 20 or later.

To move to a newer kit, copy its files over the copy (the whole of /assets/v1/, same layout), run this check, and note the
version in the pull request. Do not edit a file of the copy. Change a colour in docs/theme.css only; the palette lives nowhere else.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KIT = Path("tools/docs/site-kit/v1")
THEME = Path("docs/theme.css")
SLUG = "virtual-serial-studio"  # the checker wants the folder of theme.css to be named for the app


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def check_sums(root: Path) -> list[str]:
    problems: list[str] = []
    base = root / KIT
    sums = base / "kit" / "SHA256SUMS.txt"
    if not sums.is_file():
        return [f"{KIT}/kit/SHA256SUMS.txt is missing"]
    listed: set[str] = set()
    for line in sums.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        expected, _, name = line.partition("  ")
        listed.add(name)
        path = base / name
        if not path.is_file():
            problems.append(f"{KIT}/{name} is listed in SHA256SUMS.txt and is missing")
        elif digest(path) != expected:
            problems.append(f"{KIT}/{name} differs from the kit's SHA256SUMS.txt: it was edited. Copy the kit file again instead")
    for path in sorted(base.rglob("*")):
        name = path.relative_to(base).as_posix()
        if path.is_file() and name not in listed and name != "kit/SHA256SUMS.txt":
            problems.append(f"{KIT}/{name} is not in the kit's SHA256SUMS.txt: do not add files to the copy")
    return problems


def check_palette(root: Path) -> list[str]:
    node = shutil.which("node")
    if node is None:
        return ["node is not installed: the company's palette checker needs Node 20 or later"]
    theme = root / THEME
    if not theme.is_file():
        return [f"{THEME} is missing"]
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / SLUG
        folder.mkdir()
        shutil.copyfile(theme, folder / "theme.css")
        result = subprocess.run(
            [node, str(root / KIT / "kit" / "check-palette.mjs"), "--shell", str(root / KIT / "icl.css"), str(folder / "theme.css")],
            capture_output=True, text=True, check=False,
        )
    if result.returncode != 0:
        return [f"{THEME} does not pass the company's palette check:\n" + (result.stdout + result.stderr).rstrip()]
    return []


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    problems = check_sums(args.root) + check_palette(args.root)
    for problem in problems:
        print(f"::error::{problem}")
    if problems:
        return 1
    version = (args.root / KIT / "kit" / "VERSION.txt").read_text(encoding="utf-8").strip()
    print(f"Site kit {version} and the /virtual-serial-studio/ palette: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
