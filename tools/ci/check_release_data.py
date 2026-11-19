#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the release data of the documentation site and the update feed it publishes (docs/ci.md, "Release data and the update feed").

    python3 tools/ci/check_release_data.py [--site build/docs] [--root .] [--keys PATH]

Three things are held:

- every address in tools/docs/release/release.json begins with `<repo_url of mkdocs.yml>/releases/`, so a downloads page can only link to
  this repository's releases. (The release data is also read the way the site build reads it, so a record that the build would refuse is
  reported here with the reason.)
- when the built site has `updates/v1/feed.json`, it verifies with the keys in libs/vss_update/src/trusted_keys_release.cpp, through
  `update_feed.verify_with_keys_file`: a signature of a trusted key over a payload that Studio would accept. A feed when that file lists no
  key is an error, because no Studio would accept it. A site with no feed has nothing to check.
- the built `updates/v1/feed.json` is the same bytes as tools/docs/release/update-feed.json, and a feed in one place is a feed in the other.

Without --site only the first is checked. Each problem is printed as `::error::...`; the exit status is 1 when there is one.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
for folder in (HERE, ROOT / "tools", ROOT / "tools" / "docs"):
    if str(folder) not in sys.path:
        sys.path.insert(0, str(folder))

import site_address  # noqa: E402
import site_release_hook as hook  # noqa: E402
import update_feed  # noqa: E402

KEYS_FILE = Path("libs") / "vss_update" / "src" / "trusted_keys_release.cpp"
FEED_IN_SITE = hook.FEED_OUTPUT


def strings_of(value: object, where: str = "release.json"):  # noqa: ANN201
    """Every string in a JSON value, with where it is."""
    if isinstance(value, str):
        yield where, value
    elif isinstance(value, dict):
        for key, item in value.items():
            yield from strings_of(item, f"{where}.{key}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from strings_of(item, f"{where}[{index}]")


def check_addresses(root: Path) -> list[str]:
    folder = root / hook.RELEASE_DIR
    prefix = site_address.repo_url(root) + "/releases/"
    problems: list[str] = []
    try:
        document = json.loads((folder / hook.RELEASE_NAME).read_bytes().decode("utf-8"))
    except (OSError, ValueError) as error:
        return [f"{hook.RELEASE_DIR.as_posix()}/{hook.RELEASE_NAME}: cannot be read: {error}"]
    for where, text in strings_of(document):
        if "://" in text and not text.startswith(prefix):
            problems.append(f"{hook.RELEASE_DIR.as_posix()}/{hook.RELEASE_NAME}: {where} is '{text}', which does not begin with {prefix}")
    try:
        hook.load_release(folder / hook.RELEASE_NAME)
        hook.load_rules(folder / hook.RULES_NAME)
    except hook.ReleaseDataError as error:
        problems.append(f"{hook.RELEASE_DIR.as_posix()}: {error}")
    return problems


def check_feed(root: Path, site: Path, keys: Path | None = None) -> list[str]:
    """The feed of the built site, and its likeness to the feed of the release data."""
    built = site / FEED_IN_SITE
    source = root / hook.RELEASE_DIR / hook.FEED_NAME
    source_name = f"{hook.RELEASE_DIR.as_posix()}/{hook.FEED_NAME}"
    problems: list[str] = []
    if not built.is_file():
        if source.is_file():
            problems.append(f"{source_name} exists, but the built site has no {FEED_IN_SITE}: the site build did not publish it")
        return problems
    try:
        data = built.read_bytes()
    except OSError as error:
        return [f"{FEED_IN_SITE}: cannot read the file: {error}"]
    try:
        update_feed.verify_with_keys_file(data, FEED_IN_SITE, str(keys or root / KEYS_FILE))
    except update_feed.NoKeys:
        problems.append(f"{FEED_IN_SITE}: the site has an update feed, but the build trusts no update feed key, so no Studio would accept it")
    except update_feed.Refusal as refusal:
        problems.append(f"{FEED_IN_SITE}: {refusal.message}")
    except OSError as error:
        problems.append(f"{FEED_IN_SITE}: cannot read the keys: {error}")
    if not source.is_file():
        problems.append(f"{FEED_IN_SITE}: the built site has an update feed, but {source_name} does not exist")
    elif source.read_bytes() != data:
        problems.append(f"{FEED_IN_SITE}: is not the same bytes as {source_name}")
    return problems


def check(root: Path, site: Path | None = None, keys: Path | None = None) -> list[str]:
    problems = check_addresses(root)
    if site is not None:
        problems += check_feed(root, site, keys)
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT, help="the repository (default: this checkout)")
    parser.add_argument("--site", type=Path, help="a built site: its update feed is checked as well")
    parser.add_argument("--keys", type=Path, help=f"the C++ file of the feed keys (default: {KEYS_FILE.as_posix()} of the root)")
    args = parser.parse_args(argv)
    if args.site is not None and not args.site.is_dir():
        print(f"check_release_data: {args.site} is not a directory", file=sys.stderr)
        return 2
    try:
        problems = check(args.root, args.site, args.keys)
    except (OSError, ValueError) as error:
        print(f"check_release_data: cannot check: {error}", file=sys.stderr)
        return 2
    for problem in problems:
        print(f"::error::{problem}")
    if problems:
        return 1
    if args.site is not None and not (args.site / FEED_IN_SITE).is_file():
        print(f"Release data: ok (the site has no {FEED_IN_SITE}, so there is no feed to check)")
    else:
        print("Release data: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
