#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the files of a release before they are published.

    python3 tools/ci/release_verify.py <directory> [<public key>]

The shared publish workflow runs this, through tools/ci/release_verify.sh, as its
`verify-script`, after it has written SHA512SUMS, signed and attested, and before it creates
the release. It fails when:

* a file name has a character outside A-Z a-z 0-9 . _ -, because GitHub changes such a
  character in the name of a release asset (a "~" becomes a "."), and a user who downloads
  the files then cannot check them against SHA512SUMS;
* SHA512SUMS does not list a published file (the SBOM included), lists one that is not
  there, or has a wrong sum;
* a file has no `.asc` signature beside it;
* there is no SBOM.

The public key is accepted because the workflow passes it; the workflow itself checks the
signatures against it.
"""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

SUMS = "SHA512SUMS"
SBOM = "sbom.spdx.json"
SAFE = re.compile(r"^[A-Za-z0-9._-]+$")
LINE = re.compile(r"^([0-9a-f]{128}) [ *](.+)$")


def sum_of(path: Path) -> str:
    digest = hashlib.sha512()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def problems(directory: Path) -> list[str]:
    found: list[str] = []
    files = sorted(p for p in directory.iterdir() if p.is_file())
    if not files:
        return [f"{directory} holds no files"]
    names = {p.name for p in files}

    for name in sorted(names):
        if not SAFE.match(name):
            found.append(f"{name}: GitHub changes a character of this name in a release asset, so SHA512SUMS would not match it")

    if SUMS not in names:
        return found + [f"{SUMS} is missing"]
    if SBOM not in names:
        found.append(f"{SBOM} is missing")

    listed: dict[str, str] = {}
    for number, line in enumerate((directory / SUMS).read_text(encoding="utf-8").splitlines(), 1):
        match = LINE.match(line)
        if not match:
            found.append(f"{SUMS} line {number} is not a SHA-512 sum and a name")
            continue
        listed[match.group(2)] = match.group(1)

    for name, expected in sorted(listed.items()):
        if not SAFE.match(name):
            found.append(f"{SUMS} names {name}, which the release will not carry under that name")
        elif name not in names:
            found.append(f"{SUMS} names {name}, which is not in the release")
        elif sum_of(directory / name) != expected:
            found.append(f"{SUMS} has a wrong sum for {name}")

    published = {n for n in names if not n.endswith(".asc") and not n.startswith(SUMS)}
    for name in sorted(published - set(listed)):
        found.append(f"{SUMS} does not list {name}")

    for name in sorted(n for n in names if not n.endswith(".asc")):
        if name + ".asc" not in names:
            found.append(f"{name} has no signature ({name}.asc)")
    return found


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.split("\n\n")[0], file=sys.stderr)
        return 2
    directory = Path(argv[1])
    found = problems(directory)
    for text in found:
        print(f"error: {text}", file=sys.stderr)
    if found:
        return 1
    print(f"{directory}: the names are safe, SHA512SUMS lists every file and each file is signed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
