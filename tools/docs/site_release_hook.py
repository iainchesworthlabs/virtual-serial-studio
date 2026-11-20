# SPDX-License-Identifier: GPL-3.0-or-later
"""MkDocs hook: the newest release for the Downloads page, and the signed update feed.

The data is in tools/docs/release/ (docs/ci.md, "Release data and the update feed"):

- `release.json`: the newest release of the repository as GitHub gave it, cut down to the tag, the date, the pre-release flag,
  the address of the release and its files (name, size, address). `tools/docs/refresh_release.py` writes it.
- `download_rules.json`: how the files are sorted into groups on the Downloads page.
- `update-feed.json`: the signed update feed of the newest release that has one. It may be missing.

What the hook does:

- `on_config` reads the first two, checks every address in them (https, github.com, nothing else) and puts what pages need in
  `config.extra.release`: tag, version, date, kind, pre-release flag, address, files and the downloads table as HTML.
- `on_page_markdown` replaces these markers in a page, wherever they stand (also in a code block, so a page that talks about them
  breaks the build and is not left half-true). An unknown `releases:` marker stops the build, so a typo is not left in a page.

      <!-- releases:downloads -->   the table of files, grouped by system, with sizes and the .asc signature links (plain HTML)
      <!-- releases:version -->     0.1.0-rc.5            (the tag without its v)
      <!-- releases:tag -->         v0.1.0-rc.5
      <!-- releases:date -->        15 November 2026
      <!-- releases:kind -->        final release, release candidate or pre-release
      <!-- releases:url -->         the release on GitHub

- `on_post_build` copies `update-feed.json`, byte for byte, to `updates/v1/feed.json`, the address Studio reads (docs/updates.md,
  section 4.1). The file is never read as text and never changed. Without it the site has no updates/ folder, and the hook logs a
  plain message: a warning would fail the strict build.

The module has no MkDocs import, so tools/docs/refresh_release.py and the tests use it as it is.
"""

from __future__ import annotations

import html
import json
import logging
import re
import shutil
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

LOG = logging.getLogger("mkdocs.hooks.site_release")

RELEASE_DIR = Path("tools") / "docs" / "release"
RELEASE_NAME = "release.json"
RULES_NAME = "download_rules.json"
FEED_NAME = "update-feed.json"
FEED_OUTPUT = "updates/v1/feed.json"

# Every address on the Downloads page is a link to GitHub. tools/ci/check_release_data.py holds the addresses of release.json to this
# repository's releases; the hook only refuses what the page must never link to, and what would break the Markdown around it.
ADDRESS_RE = re.compile(r"https://github\.com/[^\s<>\"'()]+\Z")
MARKER_RE = re.compile(r"<!--\s*releases:([a-z]+)\s*-->")
DATE_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2})(?:T|$)")
NON_ALNUM_RE = re.compile(r"[^a-z0-9]+")
CANDIDATE_RE = re.compile(r"-rc\.\d+$")

SIGNATURE_SUFFIX = ".asc"
OTHER_PLATFORM = "Other files"

MONTHS = (
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
)


class ReleaseDataError(ValueError):
    """A problem with the release data that the person who edits it has to fix."""


@dataclass(frozen=True)
class Asset:
    name: str
    size: int
    url: str


@dataclass(frozen=True)
class Release:
    tag: str
    prerelease: bool
    published_at: str
    html_url: str
    assets: tuple[Asset, ...]


@dataclass(frozen=True)
class Rule:
    pattern: re.Pattern[str]
    platform: str
    label: str


# -- reading -------------------------------------------------------------------------------------------------------------


def read_json(path: Path) -> object:
    try:
        text = path.read_bytes().decode("utf-8")
    except FileNotFoundError:
        raise ReleaseDataError(f"{path} does not exist") from None
    except (OSError, UnicodeDecodeError) as error:
        raise ReleaseDataError(f"{path} cannot be read as UTF-8: {error}") from None
    try:
        return json.loads(text.removeprefix("﻿"))
    except json.JSONDecodeError as error:
        raise ReleaseDataError(f"{path} is not valid JSON: {error}") from None


def require_string(value: object, where: str) -> str:
    if not isinstance(value, str) or not value:
        raise ReleaseDataError(f"{where} must be a non-empty string")
    return value


def require_object(value: object, where: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise ReleaseDataError(f"{where} must be a JSON object")
    return value


def require_list(value: object, where: str) -> list[object]:
    if not isinstance(value, list):
        raise ReleaseDataError(f"{where} must be a JSON array")
    return value


def require_address(value: object, where: str) -> str:
    text = require_string(value, where)
    if not ADDRESS_RE.match(text):
        raise ReleaseDataError(f"{where} '{text}' must be an https://github.com/ address with no space, quote, bracket or parenthesis")
    return text


def load_release(path: Path) -> Release:
    data = require_object(read_json(path), str(path))
    prerelease = data.get("prerelease")
    if not isinstance(prerelease, bool):
        raise ReleaseDataError(f"{path}: 'prerelease' must be true or false")
    assets: list[Asset] = []
    seen: set[str] = set()
    for index, raw in enumerate(require_list(data.get("assets"), f"{path}: 'assets'")):
        entry = require_object(raw, f"{path}: assets[{index}]")
        name = require_string(entry.get("name"), f"{path}: assets[{index}].name")
        size = entry.get("size")
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise ReleaseDataError(f"{path}: assets[{index}].size must be a non-negative integer")
        if name in seen:
            raise ReleaseDataError(f"{path}: asset '{name}' appears twice")
        seen.add(name)
        assets.append(Asset(name, size, require_address(entry.get("url"), f"{path}: assets[{index}].url")))
    release = Release(
        tag=require_string(data.get("tag"), f"{path}: 'tag'"),
        prerelease=prerelease,
        published_at=require_string(data.get("published_at"), f"{path}: 'published_at'"),
        html_url=require_address(data.get("html_url"), f"{path}: 'html_url'"),
        assets=tuple(assets),
    )
    format_date(release.published_at)
    return release


def load_rules(path: Path) -> tuple[Rule, ...]:
    data = require_object(read_json(path), str(path))
    rules: list[Rule] = []
    for index, raw in enumerate(require_list(data.get("rules"), f"{path}: 'rules'")):
        where = f"{path}: rules[{index}]"
        entry = require_object(raw, where)
        expression = require_string(entry.get("match"), f"{where}.match")
        try:
            pattern = re.compile(expression)
        except re.error as error:
            raise ReleaseDataError(f"{where}.match is not a valid regular expression: {error}") from None
        rules.append(Rule(pattern, require_string(entry.get("platform"), f"{where}.platform"), require_string(entry.get("label"), f"{where}.label")))
    return tuple(rules)


# -- formatting ----------------------------------------------------------------------------------------------------------


def escape(value: str) -> str:
    return html.escape(value, quote=True)


def format_size(size: int) -> str:
    """A byte count as MB (one decimal) or kB (none). 1 MB is 1,000,000 bytes."""
    if size >= 1_000_000:
        tenths = (size + 50_000) // 100_000
        return f"{tenths // 10}.{tenths % 10} MB"
    kilobytes = (size + 500) // 1000
    if size > 0:
        kilobytes = max(kilobytes, 1)
    return f"{kilobytes} kB"


def format_date(published_at: str) -> str:
    """An ISO timestamp as, for example, `12 November 2026`."""
    match = DATE_RE.match(published_at)
    if not match:
        raise ReleaseDataError(f"published_at '{published_at}' does not start with YYYY-MM-DD")
    year, month, day = (int(part) for part in match.groups())
    if not 1 <= month <= 12 or not 1 <= day <= 31:
        raise ReleaseDataError(f"published_at '{published_at}' is not a valid date")
    return f"{day} {MONTHS[month - 1]} {year}"


def kind_of(release: Release) -> str:
    if not release.prerelease:
        return "final release"
    return "release candidate" if CANDIDATE_RE.search(release.tag) else "pre-release"


def platform_id(platform: str) -> str:
    slug = NON_ALNUM_RE.sub("-", platform.lower()).strip("-")
    return "dl-" + (slug or "files")


# -- the downloads table -------------------------------------------------------------------------------------------------


def render_downloads(release: Release, rules: tuple[Rule, ...]) -> str:
    """One section for each group of files, in the order of the rules, as plain HTML. A `.asc` file whose signed file is in the
    release is the Signature link of that file, not a row. A file that no rule fits is in the last group, Other files."""
    names = {asset.name for asset in release.assets}
    signatures: dict[str, Asset] = {}
    files: list[Asset] = []
    for asset in release.assets:
        base = asset.name.removesuffix(SIGNATURE_SUFFIX)
        if asset.name.endswith(SIGNATURE_SUFFIX) and base in names:
            signatures[base] = asset
        else:
            files.append(asset)

    order = list(dict.fromkeys(rule.platform for rule in rules))
    if OTHER_PLATFORM not in order:
        order.append(OTHER_PLATFORM)

    groups: dict[str, list[tuple[int, str, str, Asset]]] = {platform: [] for platform in order}
    for asset in files:
        for index, rule in enumerate(rules):
            if rule.pattern.search(asset.name):
                groups[rule.platform].append((index, asset.name, rule.label, asset))
                break
        else:
            groups[OTHER_PLATFORM].append((len(rules), asset.name, asset.name, asset))

    sections: list[str] = []
    for platform in order:
        rows = sorted(groups[platform], key=lambda row: (row[0], row[1]))
        if not rows:
            continue
        identifier = escape(platform_id(platform))
        title = escape(platform)
        lines = [
            f'<section class="vss-dl-group" aria-labelledby="{identifier}">',
            f'<h2 id="{identifier}">{title}</h2>',
            '<table class="vss-dl">',
            f'<caption class="icl-sr">{title} downloads</caption>',
            '<thead><tr><th scope="col">File</th><th scope="col">Size</th><th scope="col">Signature</th></tr></thead>',
            "<tbody>",
        ]
        for _, name, label, asset in rows:
            signature = signatures.get(name)
            if signature is None:
                last = "<td>None</td>"
            else:
                last = f'<td><a href="{escape(signature.url)}" aria-label="Signature for {escape(name)}">.asc</a></td>'
            lines.append(
                f'<tr><th scope="row"><a href="{escape(asset.url)}">{escape(label)}</a>'
                f'<span class="vss-dl__name">{escape(name)}</span></th>'
                f"<td>{format_size(asset.size)}</td>{last}</tr>"
            )
        lines += ["</tbody>", "</table>", "</section>"]
        sections.append("\n".join(lines))
    return "\n".join(sections)


# -- the markers ---------------------------------------------------------------------------------------------------------


def describe(release: Release, rules: tuple[Rule, ...]) -> dict[str, object]:
    """What pages get as `config.extra.release` and the markers are made from. The strings are HTML-escaped, except `downloads`, which
    is HTML already."""
    return {
        "tag": escape(release.tag),
        "version": escape(release.tag.removeprefix("v")),
        "date": escape(format_date(release.published_at)),
        "kind": kind_of(release),
        "prerelease": release.prerelease,
        "url": escape(release.html_url),
        "assets": [{"name": a.name, "size": a.size, "url": a.url} for a in release.assets],
        "downloads": render_downloads(release, rules),
    }


# The keys of `describe` that a marker may name.
MARKERS = frozenset({"downloads", "version", "tag", "date", "kind", "url"})


def fill(markdown: str, release: Mapping[str, object]) -> str:
    """Replace the `<!-- releases:... -->` markers in a page's Markdown."""

    def replace(match: re.Match[str]) -> str:
        key = match.group(1)
        if key not in MARKERS:
            raise ReleaseDataError(f"the marker <!-- releases:{key} --> is not known; the markers are {', '.join(sorted(MARKERS))}")
        return str(release[key])

    return MARKER_RE.sub(replace, markdown)


def root_of(config: Mapping[str, object]) -> Path:
    return Path(str(config["docs_dir"])).resolve().parent


# -- the hook ------------------------------------------------------------------------------------------------------------


def on_config(config):  # noqa: ANN001, ANN201 (MkDocs passes its own types)
    folder = root_of(config) / RELEASE_DIR
    release = load_release(folder / RELEASE_NAME)
    rules = load_rules(folder / RULES_NAME)
    config["extra"]["release"] = describe(release, rules)
    return config


def on_page_markdown(markdown, page, config, files):  # noqa: ANN001, ANN201
    if "releases:" not in markdown:
        return markdown
    return fill(markdown, config["extra"]["release"])


def publish_feed(source: Path, site_dir: Path) -> bool:
    """Copy the feed, as the bytes it is, to the address Studio reads. False when there is no feed."""
    if not source.is_file():
        return False
    target = site_dir / FEED_OUTPUT
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    return True


def on_post_build(config):  # noqa: ANN001, ANN201
    source = root_of(config) / RELEASE_DIR / FEED_NAME
    if publish_feed(source, Path(str(config["site_dir"]))):
        LOG.info("%s is the update feed: copied unchanged to %s", FEED_NAME, FEED_OUTPUT)
    else:
        LOG.info("%s/%s does not exist, so the site has no %s", RELEASE_DIR.as_posix(), FEED_NAME, FEED_OUTPUT)
