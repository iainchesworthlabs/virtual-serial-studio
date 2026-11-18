#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep the Windows driver's version in step with what the driver is made of.

    python3 tools/ci/check_driver_version.py            # check; exit 1 and say what to do when it is wrong
    python3 tools/ci/check_driver_version.py --bump     # a new revision: after you changed the driver
    python3 tools/ci/check_driver_version.py --base <ref>   # also compare with the version on another ref

The one source of truth is drivers/windows/driver-version.json: a revision number that only goes up, the date
of the revision, and the SHA-256 of everything the driver is made of (the sources, the INF files without their
DriverVer line, the public interface header and the helper's protocol). The version Windows sees is built from it:

    DriverVer=<date>,1.<interface>.<revision>.0

where <interface> is VSSCOM_VERSION in drivers/windows/vsscom/public.h. Windows decides which of two packages of
one driver is newer by the date first and the four-part version second, and `pnputil /add-driver` answers
"Already exists in the system" for one that is not newer, so a driver that changed without a new version never
reaches a PC that has the old one. This script fails in that case:

  * the inputs hash differs from the recorded one: the driver changed and the revision was not bumped;
  * an INF file carries another DriverVer than the one the revision gives;
  * the interface number in the file is not VSSCOM_VERSION;
  * with --base: the inputs differ from those on the base, and the revision did not rise or the date went back.

--bump is the only thing that writes the file. It raises the revision by one, sets the date to today (never
earlier than the recorded date, as the date must not go backwards) and rewrites the DriverVer line of the INFs.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVERS = Path("drivers/windows")
VERSION_FILE = DRIVERS / "driver-version.json"
PUBLIC_HEADER = DRIVERS / "vsscom/public.h"
INFS = (DRIVERS / "vsscom/vsscom.inf", DRIVERS / "vsscom/vsscomctl.inf", DRIVERS / "vsscom_um/vsscom_um.inf")
# What the driver is made of. Setup scripts and the installer are not in it: they do not change the driver.
INPUT_DIRS = (DRIVERS / "vsscom", DRIVERS / "vsscom_um", DRIVERS / "vsscom_helper")
SKIP_DIRS = {"artifacts", "obj", "bin", "x64", "arm64", "debug", "release", ".vs", "packages"}
SKIP_SUFFIXES = {".user", ".log", ".tlog"}
DRIVER_VER = re.compile(r"^(\s*DriverVer\s*=\s*)([^\r\n]*)", re.MULTILINE)
DATE = "%m/%d/%Y"


def input_files(root: Path) -> list[Path]:
    found: list[Path] = []
    for directory in INPUT_DIRS:
        for path in sorted((root / directory).rglob("*")):
            relative = path.relative_to(root / directory)
            if not path.is_file() or any(part.lower() in SKIP_DIRS for part in relative.parts[:-1]):
                continue
            if path.suffix.lower() in SKIP_SUFFIXES:
                continue
            found.append(path)
    return found


def normalised(path: Path) -> bytes:
    data = path.read_bytes().replace(b"\r\n", b"\n")
    if path.suffix.lower() == ".inf":
        text = DRIVER_VER.sub(lambda m: m.group(1), data.decode("utf-8", errors="replace"))
        data = text.encode("utf-8")
    return data


def inputs_hash(root: Path) -> str:
    outer = hashlib.sha256()
    for path in input_files(root):
        name = path.relative_to(root).as_posix()
        outer.update(name.encode() + b"\0" + hashlib.sha256(normalised(path)).digest())
    return outer.hexdigest()


def interface_number(root: Path) -> int:
    match = re.search(r"^#define\s+VSSCOM_VERSION\s+(\d+)\s*$", (root / PUBLIC_HEADER).read_text(encoding="utf-8"), re.MULTILINE)
    if not match:
        raise SystemExit(f"{PUBLIC_HEADER}: no #define VSSCOM_VERSION found")
    return int(match.group(1))


def driver_ver(record: dict, interface: int) -> str:
    return f"{record['date']},1.{interface}.{record['revision']}.0"


def load(text: str) -> dict:
    record = json.loads(text)
    for key, kind in (("revision", int), ("date", str), ("inputs_sha256", str)):
        if not isinstance(record.get(key), kind):
            raise ValueError(f"driver-version.json: '{key}' is missing or not a {kind.__name__}")
    datetime.datetime.strptime(record["date"], DATE)
    return record


def parse_date(text: str) -> datetime.date:
    return datetime.datetime.strptime(text, DATE).date()


def check(root: Path, base_text: str | None = None) -> list[str]:
    """Every way the driver's version is wrong, as sentences that say what to do."""
    fix = "run: python3 tools/ci/check_driver_version.py --bump"
    try:
        record = load((root / VERSION_FILE).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return [f"{VERSION_FILE}: {error}"]
    problems: list[str] = []
    interface = interface_number(root)
    wanted = driver_ver(record, interface)
    if inputs_hash(root) != record["inputs_sha256"]:
        problems.append(
            "the driver (sources, INF files, public.h or the helper protocol under drivers/windows/vsscom, vsscom_um "
            f"or vsscom_helper) changed and its revision was not raised, so a PC that has the old driver would keep it; {fix}"
        )
    for inf in INFS:
        match = DRIVER_VER.search((root / inf).read_text(encoding="utf-8"))
        have = match.group(2).strip() if match else "(none)"
        if have != wanted:
            problems.append(f"{inf.as_posix()} has DriverVer={have} and driver-version.json says {wanted}; {fix}")
    if base_text is not None:
        try:
            base = load(base_text)
        except ValueError:
            base = None
        if base and base["inputs_sha256"] != record["inputs_sha256"]:
            if record["revision"] <= base["revision"]:
                problems.append(
                    f"the driver differs from the base but its revision is {record['revision']}, "
                    f"not above the base's {base['revision']}; {fix}"
                )
            if parse_date(record["date"]) < parse_date(base["date"]):
                problems.append(f"the driver's date {record['date']} is before the base's {base['date']}; Windows compares the date first")
    return problems


def bump(root: Path, today: datetime.date | None = None) -> str:
    today = today or datetime.datetime.now(datetime.timezone.utc).date()
    record = load((root / VERSION_FILE).read_text(encoding="utf-8"))
    record["revision"] += 1
    record["date"] = max(today, parse_date(record["date"])).strftime(DATE)
    record["inputs_sha256"] = inputs_hash(root)
    wanted = driver_ver(record, interface_number(root))
    for inf in INFS:
        path = root / inf
        text = path.read_bytes().decode("utf-8")
        text, count = DRIVER_VER.subn(lambda m: m.group(1) + wanted, text)
        if count != 1:
            raise SystemExit(f"{inf}: expected one DriverVer line, found {count}")
        path.write_bytes(text.encode("utf-8"))
    (root / VERSION_FILE).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return wanted


def base_version(root: Path, ref: str) -> str | None:
    result = subprocess.run(["git", "-C", str(root), "show", f"{ref}:{VERSION_FILE.as_posix()}"],
                            capture_output=True, text=True, check=False)
    return result.stdout if result.returncode == 0 else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--base", help="a git ref whose driver version the new one must be above when the driver changed")
    parser.add_argument("--bump", action="store_true", help="raise the revision and rewrite the INF files")
    args = parser.parse_args(argv)
    if args.bump:
        print(f"driver version is now DriverVer={bump(args.root)}")
        return 0
    problems = check(args.root, base_version(args.root, args.base) if args.base else None)
    for problem in problems:
        print(f"driver version: {problem}", file=sys.stderr)
    if not problems:
        print("driver version: ok")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
