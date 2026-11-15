#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if the owner's licence issuing tool, or key material, is in a package.

`vss_license` issues licences, signs revocation lists and makes keys. It is
the owner's tool. No installer, archive, disk image, deb or rpm may contain
it; customers use Help, Licence in Studio or `vss_cli licence`. The library
that holds the published test key, and the one that signs, are for the tool
and the tests and must not ship either, and no key file may ship at all.

    python3 tools/ci/check_package_contents.py --source .
    python3 tools/ci/check_package_contents.py --tree stage
    python3 tools/ci/check_package_contents.py --listing packages.txt

`--source` reads the CMake files of a checkout and fails if an `install()`
rule names one of the licence tool's targets. `--tree` walks a directory that
was installed or staged for packaging. `--listing` reads paths, one on each
line, as `dpkg-deb --contents`, `rpm -qpl` and `tar -tf` print them (the path
is the last field of a line). Each option may be given more than once. The
exit status is 1 when anything forbidden is found, 2 for a usage error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path, PurePosixPath

# The program, as CMake names its output on each system.
FORBIDDEN_PROGRAMS = {"vss_license", "vss_license.exe"}
# Libraries that exist for the issuing tool and the tests only.
FORBIDDEN_NAME_PARTS = ("vss_license_test_keys", "vss_license_sign", "vss_license_tool")
# Key material never ships, whatever it is called.
FORBIDDEN_SUFFIXES = {".key", ".pem", ".seed", ".p12", ".pfx"}
# The CMake targets an install() rule must never name.
FORBIDDEN_TARGETS = ("vss_license_program", "vss_license_tool", "vss_license_test_keys", "vss_license_sign")
# Folders of a checkout that hold no install rules of this project.
SKIPPED_SOURCE_DIRS = {"build", ".git", ".cache", "third_party", "node_modules"}


def why_forbidden(path: str) -> str | None:
    """The reason a packaged path is not allowed, or None when it is fine."""
    name = PurePosixPath(path.replace("\\", "/").rstrip("/")).name.lower()
    if not name:
        return None
    if name in FORBIDDEN_PROGRAMS:
        return "the licence issuing tool"
    for part in FORBIDDEN_NAME_PARTS:
        if part in name:
            return "a library of the licence issuing tool"
    for suffix in FORBIDDEN_SUFFIXES:
        if name.endswith(suffix):
            return "key material"
    return None


def check_paths(paths: list[str]) -> list[str]:
    """One message for each forbidden path, in the order given."""
    problems = []
    for path in paths:
        reason = why_forbidden(path)
        if reason:
            problems.append(f"{path}: {reason} must not be in a package")
    return problems


def tree_paths(root: Path) -> list[str]:
    return sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file() or p.is_symlink())


def listing_paths(text: str) -> list[str]:
    """The path of each line of a package listing: its last field."""
    paths = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        # `dpkg-deb --contents` and `tar -tvf` end a symbolic link with
        # " -> target": the path is what comes before it.
        line = line.split(" -> ")[0]
        paths.append(line.split()[-1] if " " in line else line)
    return paths


def install_calls(text: str) -> list[tuple[int, str]]:
    """Every `install(...)` call of a CMake file, with the line it starts on."""
    calls = []
    for match in re.finditer(r"(?im)^[ \t]*install[ \t]*\(", text):
        depth = 0
        end = match.end() - 1
        for index in range(end, len(text)):
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
                if depth == 0:
                    end = index
                    break
        calls.append((text.count("\n", 0, match.start()) + 1, text[match.start():end + 1]))
    return calls


def check_source(root: Path) -> list[str]:
    """Problems in the install rules of a checkout."""
    problems = []
    files = [p for p in list(root.rglob("CMakeLists.txt")) + list(root.rglob("*.cmake"))
             if not (set(p.relative_to(root).parts[:-1]) & SKIPPED_SOURCE_DIRS)]
    for path in sorted(files):
        text = path.read_text(encoding="utf-8", errors="replace")
        shown = path.relative_to(root).as_posix()
        for line, call in install_calls(text):
            for target in FORBIDDEN_TARGETS:
                if re.search(r"\b" + re.escape(target) + r"\b", call):
                    problems.append(f"{shown}:{line}: an install() rule names {target}; "
                                    "the licence issuing tool is not installed")
        # The list of library targets the top-level file installs.
        for match in re.finditer(r"(?is)foreach\s*\(\s*_vss_target\b(.*?)\)", text):
            for target in FORBIDDEN_TARGETS + ("vss_license",):
                if re.search(r"\b" + re.escape(target) + r"\b", match.group(1)):
                    line = text.count("\n", 0, match.start()) + 1
                    problems.append(f"{shown}:{line}: the installed targets include {target}")
    tool = root / "apps" / "vss_license" / "CMakeLists.txt"
    if tool.is_file() and install_calls(tool.read_text(encoding="utf-8", errors="replace")):
        problems.append("apps/vss_license/CMakeLists.txt: the licence issuing tool has an install() rule")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", action="append", default=[], help="a checkout whose install rules are read")
    parser.add_argument("--tree", action="append", default=[], help="an installed or staged directory")
    parser.add_argument("--listing", action="append", default=[], help="a file of package paths, one on each line")
    args = parser.parse_args(argv)
    if not (args.source or args.tree or args.listing):
        parser.error("give --source, --tree or --listing")

    problems: list[str] = []
    checked = 0
    for source in args.source:
        root = Path(source)
        if not root.is_dir():
            parser.error(f"{source} is not a directory")
        problems += check_source(root)
    for tree in args.tree:
        root = Path(tree)
        if not root.is_dir():
            parser.error(f"{tree} is not a directory")
        paths = tree_paths(root)
        checked += len(paths)
        problems += [f"{tree}/{p}" for p in check_paths(paths)]
    for listing in args.listing:
        file = Path(listing)
        if not file.is_file():
            parser.error(f"{listing} is not a file")
        paths = listing_paths(file.read_text(encoding="utf-8", errors="replace"))
        checked += len(paths)
        problems += check_paths(paths)

    for problem in problems:
        print(f"FAIL  {problem}")
    if problems:
        return 1
    print(f"PASS  no licence issuing tool and no key material ({checked} packaged path(s) read)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
