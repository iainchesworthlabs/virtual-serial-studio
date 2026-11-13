#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the static product website from ``website/``.

The builder is a small, deterministic generator that uses only the Python
standard library.  The same input always produces the same bytes: there are no
timestamps, every file uses LF line endings and UTF-8, and files are written in
sorted order.

Inputs, relative to the repository root:

``website/site.json``
    The site name, base URL, repository, navigation, files to copy and the rules
    that sort release assets into download groups.
``website/templates/base.html``
    The page layout.  It must contain the ``{{content}}`` placeholder.
``website/pages/*.html``
    One file per page.  Each starts with a front-matter HTML comment holding
    ``title`` and ``description`` (required) and ``nav`` and ``sitemap``
    (optional), followed by the body fragment.
``website/static/**``
    Copied to the output root.  ``static/css/site.css`` is appended to the
    generated colour tokens instead.
``website/data/release.json``
    The latest release, as written by ``--refresh-release``.

Placeholders have the form ``{{key}}`` with lower-case letters and underscores.
Values that come from data are HTML-escaped; ``content``, ``nav`` and
``downloads`` are HTML.  An unknown key, or a placeholder left over after
substitution, stops the build.

Usage
-----
    python tools/site_build.py                    # build into build/website
    python tools/site_build.py --out some/dir     # build somewhere else
    python tools/site_build.py --refresh-release  # fetch the latest release first

``--refresh-release`` is the only option that uses the network.  It sends a
single request to the GitHub API and rewrites ``website/data/release.json``.
"""

from __future__ import annotations

import argparse
import html
import json
import os
import re
import shutil
import sys
import urllib.error
import urllib.request
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = Path("build") / "website"

PLACEHOLDER_RE = re.compile(r"\{\{([a-z_]+)\}\}")
COLOUR_RE = re.compile(r"#[0-9A-Fa-f]{6}")
TOKEN_NAME_RE = re.compile(r"[A-Za-z][A-Za-z0-9]*")
CAMEL_BOUNDARY_RE = re.compile(r"(?<=[a-z0-9])(?=[A-Z])")
NON_ALNUM_RE = re.compile(r"[^a-z0-9]+")
DATE_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2})(?:T|$)")

FRONT_MATTER_KEYS = ("title", "description", "nav", "sitemap")
INDEX_FILE = "index.html"
OTHER_PLATFORM = "Other files"
SIGNATURE_SUFFIX = ".asc"

MONTHS = (
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
)

API_USER_AGENT = "virtual-serial-studio-site-build"
API_TIMEOUT_SECONDS = 30


class BuildError(Exception):
    """A problem with the site sources that the author has to fix."""


@dataclass(frozen=True)
class NavEntry:
    file: str
    label: str


@dataclass(frozen=True)
class CopyEntry:
    source: str
    target: str


@dataclass(frozen=True)
class DownloadRule:
    pattern: re.Pattern[str]
    platform: str
    label: str


@dataclass(frozen=True)
class SiteConfig:
    name: str
    base_url: str
    repo: str
    tokens: str
    nav: tuple[NavEntry, ...]
    copy: tuple[CopyEntry, ...]
    rules: tuple[DownloadRule, ...]


@dataclass(frozen=True)
class Page:
    file: str
    title: str
    description: str
    nav: str | None
    sitemap: bool
    body: str


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
class SiteOutput:
    """Everything a build produces, held in memory before anything is written."""

    files: dict[str, bytes]
    inputs: list[Path]
    page_count: int


# -- reading ---------------------------------------------------------------


def normalise_newlines(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def read_text(path: Path) -> str:
    """Reads a UTF-8 file and returns its text with LF newlines."""
    try:
        text = path.read_bytes().decode("utf-8")
    except FileNotFoundError:
        raise BuildError(f"{path} does not exist") from None
    except (OSError, UnicodeDecodeError) as error:
        raise BuildError(f"{path} cannot be read as UTF-8: {error}") from None
    return normalise_newlines(text.removeprefix("﻿"))


def read_json(path: Path) -> object:
    try:
        return json.loads(read_text(path))
    except json.JSONDecodeError as error:
        raise BuildError(f"{path} is not valid JSON: {error}") from None


def require_string(value: object, where: str) -> str:
    if not isinstance(value, str) or not value:
        raise BuildError(f"{where} must be a non-empty string")
    return value


def require_object(value: object, where: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise BuildError(f"{where} must be a JSON object")
    return value


def require_list(value: object, where: str) -> list[object]:
    if not isinstance(value, list):
        raise BuildError(f"{where} must be a JSON array")
    return value


def load_config(source: Path) -> SiteConfig:
    path = source / "site.json"
    data = require_object(read_json(path), str(path))

    def field(key: str) -> str:
        return require_string(data.get(key), f"{path}: '{key}'")

    base_url = field("base_url")
    if not base_url.endswith("/"):
        raise BuildError(f"{path}: 'base_url' must end with a slash")

    nav: list[NavEntry] = []
    for index, raw in enumerate(require_list(data.get("nav", []), f"{path}: 'nav'")):
        entry = require_object(raw, f"{path}: nav[{index}]")
        nav.append(
            NavEntry(
                require_string(entry.get("file"), f"{path}: nav[{index}].file"),
                require_string(entry.get("label"), f"{path}: nav[{index}].label"),
            )
        )

    copy: list[CopyEntry] = []
    for index, raw in enumerate(require_list(data.get("copy", []), f"{path}: 'copy'")):
        entry = require_object(raw, f"{path}: copy[{index}]")
        copy.append(
            CopyEntry(
                require_string(entry.get("from"), f"{path}: copy[{index}].from"),
                require_string(entry.get("to"), f"{path}: copy[{index}].to"),
            )
        )

    rules: list[DownloadRule] = []
    raw_rules = require_list(data.get("download_rules", []), f"{path}: 'download_rules'")
    for index, raw in enumerate(raw_rules):
        entry = require_object(raw, f"{path}: download_rules[{index}]")
        where = f"{path}: download_rules[{index}]"
        expression = require_string(entry.get("match"), f"{where}.match")
        try:
            pattern = re.compile(expression)
        except re.error as error:
            raise BuildError(f"{where}.match is not a valid regular expression: {error}") from None
        rules.append(
            DownloadRule(
                pattern,
                require_string(entry.get("platform"), f"{where}.platform"),
                require_string(entry.get("label"), f"{where}.label"),
            )
        )

    return SiteConfig(
        name=field("name"),
        base_url=base_url,
        repo=field("repo"),
        tokens=field("tokens"),
        nav=tuple(nav),
        copy=tuple(copy),
        rules=tuple(rules),
    )


def parse_page(file: str, text: str) -> Page:
    """Splits a page file into its front matter and body fragment."""
    text = normalise_newlines(text.removeprefix("﻿"))
    if not text.startswith("<!--"):
        raise BuildError(f"{file}: the page must start with a front-matter comment (<!-- ... -->)")
    end = text.find("-->")
    if end < 0:
        raise BuildError(f"{file}: the front-matter comment is not closed")

    values: dict[str, str] = {}
    for line in text[len("<!--"):end].splitlines():
        line = line.strip()
        if not line:
            continue
        key, separator, value = line.partition(":")
        key = key.strip()
        if not separator:
            raise BuildError(f"{file}: front matter line '{line}' is not 'key: value'")
        if key not in FRONT_MATTER_KEYS:
            raise BuildError(
                f"{file}: unknown front-matter key '{key}'; expected one of "
                + ", ".join(FRONT_MATTER_KEYS)
            )
        if key in values:
            raise BuildError(f"{file}: front-matter key '{key}' appears twice")
        values[key] = value.strip()

    for key in ("title", "description"):
        if not values.get(key):
            raise BuildError(f"{file}: front-matter key '{key}' is required and must not be empty")
    if "nav" in values and not values["nav"]:
        raise BuildError(f"{file}: front-matter key 'nav' must not be empty")
    sitemap_value = values.get("sitemap", "yes")
    if sitemap_value not in ("yes", "no"):
        raise BuildError(f"{file}: front-matter key 'sitemap' must be 'yes' or 'no'")

    body = text[end + len("-->"):].lstrip("\n")
    if body and not body.endswith("\n"):
        body += "\n"
    return Page(
        file=file,
        title=values["title"],
        description=values["description"],
        nav=values.get("nav"),
        sitemap=sitemap_value == "yes",
        body=body,
    )


def load_pages(source: Path) -> tuple[list[Page], list[Path]]:
    directory = source / "pages"
    paths = sorted(directory.glob("*.html")) if directory.is_dir() else []
    if not paths:
        raise BuildError(f"{directory} contains no pages")
    return [parse_page(path.name, read_text(path)) for path in paths], paths


def load_release(source: Path) -> Release:
    path = source / "data" / "release.json"
    data = require_object(read_json(path), str(path))
    prerelease = data.get("prerelease")
    if not isinstance(prerelease, bool):
        raise BuildError(f"{path}: 'prerelease' must be true or false")

    assets: list[Asset] = []
    seen: set[str] = set()
    for index, raw in enumerate(require_list(data.get("assets"), f"{path}: 'assets'")):
        entry = require_object(raw, f"{path}: assets[{index}]")
        name = require_string(entry.get("name"), f"{path}: assets[{index}].name")
        size = entry.get("size")
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise BuildError(f"{path}: assets[{index}].size must be a non-negative integer")
        if name in seen:
            raise BuildError(f"{path}: asset '{name}' appears twice")
        seen.add(name)
        url = require_string(entry.get("url"), f"{path}: assets[{index}].url")
        assets.append(Asset(name, size, url))

    return Release(
        tag=require_string(data.get("tag"), f"{path}: 'tag'"),
        prerelease=prerelease,
        published_at=require_string(data.get("published_at"), f"{path}: 'published_at'"),
        html_url=require_string(data.get("html_url"), f"{path}: 'html_url'"),
        assets=tuple(assets),
    )


# -- formatting ------------------------------------------------------------


def escape(value: str) -> str:
    return html.escape(value, quote=True)


def format_size(size: int) -> str:
    """Formats a byte count as MB (one decimal) or kB (none); 1 MB is 1,000,000 bytes."""
    if size >= 1_000_000:
        tenths = (size + 50_000) // 100_000
        return f"{tenths // 10}.{tenths % 10} MB"
    kilobytes = (size + 500) // 1000
    if size > 0:
        kilobytes = max(kilobytes, 1)
    return f"{kilobytes} kB"


def format_date(published_at: str) -> str:
    """Formats an ISO timestamp as, for example, ``12 November 2026``."""
    match = DATE_RE.match(published_at)
    if not match:
        raise BuildError(f"published_at '{published_at}' does not start with YYYY-MM-DD")
    year, month, day = (int(part) for part in match.groups())
    if not 1 <= month <= 12 or not 1 <= day <= 31:
        raise BuildError(f"published_at '{published_at}' is not a valid date")
    return f"{day} {MONTHS[month - 1]} {year}"


def platform_id(platform: str) -> str:
    slug = NON_ALNUM_RE.sub("-", platform.lower()).strip("-")
    return "dl-" + (slug or "files")


def kebab_case(name: str) -> str:
    """Turns ``accentStrong`` into ``accent-strong``; digits stay attached."""
    return CAMEL_BOUNDARY_RE.sub("-", name).lower()


# -- generated fragments ---------------------------------------------------


def render_nav(entries: tuple[NavEntry, ...], current: str | None) -> str:
    items = []
    for entry in entries:
        marker = ' aria-current="page"' if entry.file == current else ""
        items.append(f'<li><a href="{escape(entry.file)}"{marker}>{escape(entry.label)}</a></li>')
    return "\n".join(items)


def render_downloads(release: Release, rules: tuple[DownloadRule, ...]) -> str:
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
            f'<section class="dl-group" aria-labelledby="{identifier}">',
            f'<h2 id="{identifier}">{title}</h2>',
            '<table class="dl">',
            f'<caption class="visually-hidden">{title} downloads</caption>',
            '<thead><tr><th scope="col">File</th><th scope="col">Size</th>'
            '<th scope="col">Signature</th></tr></thead>',
            "<tbody>",
        ]
        for _, name, label, asset in rows:
            signature = signatures.get(name)
            if signature is None:
                last = "<td>None</td>"
            else:
                last = (
                    f'<td><a href="{escape(signature.url)}" '
                    f'aria-label="Signature for {escape(name)}">.asc</a></td>'
                )
            lines.append(
                f'<tr><th scope="row"><a href="{escape(asset.url)}">{escape(label)}</a>'
                f'<span class="dl-name">{escape(name)}</span></th>'
                f"<td>{format_size(asset.size)}</td>{last}</tr>"
            )
        lines += ["</tbody>", "</table>", "</section>"]
        sections.append("\n".join(lines))
    return "\n".join(sections)


def render_tokens_css(tokens: object, tokens_path: str) -> str:
    """Builds the generated custom-property block from the ``color`` tokens."""
    root = require_object(tokens, tokens_path)
    colour = require_object(root.get("color"), f"{tokens_path}: 'color'")

    def declarations(scheme: str) -> str:
        values = require_object(colour.get(scheme), f"{tokens_path}: 'color.{scheme}'")
        parts = []
        for name, value in values.items():
            if not TOKEN_NAME_RE.fullmatch(name):
                raise BuildError(f"{tokens_path}: colour token name '{name}' is not camelCase")
            if not isinstance(value, str) or not COLOUR_RE.fullmatch(value):
                raise BuildError(
                    f"{tokens_path}: colour token '{scheme}.{name}' must be #RRGGBB, got {value!r}"
                )
            parts.append(f"--{kebab_case(name)}:{value}")
        return ";".join(parts)

    light = declarations("light")
    dark = declarations("dark")
    lines = [
        "/* Generated by tools/site_build.py from " + tokens_path + ". Do not edit. */",
        ":root{color-scheme:light dark;" + light + "}",
        "@media (prefers-color-scheme:dark){:root{" + dark + "}}",
    ]
    return "\n".join(lines)


def render_sitemap(config: SiteConfig, pages: list[Page]) -> str:
    listed = sorted(
        (page for page in pages if page.sitemap),
        key=lambda page: (page.file != INDEX_FILE, page.file),
    )
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">',
    ]
    for page in listed:
        lines.append(f"<url><loc>{escape(canonical_url(config, page.file))}</loc></url>")
    lines.append("</urlset>")
    return "\n".join(lines) + "\n"


# -- pages -----------------------------------------------------------------


def canonical_url(config: SiteConfig, file: str) -> str:
    return config.base_url if file == INDEX_FILE else config.base_url + file


def substitute(text: str, values: Mapping[str, str], where: str) -> str:
    def replace(match: re.Match[str]) -> str:
        key = match.group(1)
        if key not in values:
            raise BuildError(f"{where}: unknown placeholder {{{{{key}}}}}")
        return values[key]

    return PLACEHOLDER_RE.sub(replace, text)


def render_page(
    page: Page,
    template: str,
    config: SiteConfig,
    release: Release,
    downloads: str,
) -> str:
    nav_files = {entry.file for entry in config.nav}
    if page.nav is not None and page.nav != page.file and page.nav not in nav_files:
        raise BuildError(f"{page.file}: nav '{page.nav}' is not in the navigation list")
    current = page.nav if page.nav is not None else page.file

    head_title = page.title if page.file == INDEX_FILE else f"{page.title} | {config.name}"
    values: dict[str, str] = {
        "name": escape(config.name),
        "title": escape(page.title),
        "head_title": escape(head_title),
        "description": escape(page.description),
        "canonical": escape(canonical_url(config, page.file)),
        "repo_url": escape(f"https://github.com/{config.repo}"),
        "nav": render_nav(config.nav, current),
        "release_tag": escape(release.tag),
        "release_version": escape(release.tag.removeprefix("v")),
        "release_url": escape(release.html_url),
        "release_kind": "Pre-release" if release.prerelease else "Release",
        "release_date": escape(format_date(release.published_at)),
        "downloads": downloads,
    }

    content = substitute(page.body, values, f"pages/{page.file}")
    output = substitute(template, {**values, "content": content}, "templates/base.html")
    leftover = PLACEHOLDER_RE.search(output)
    if leftover:
        raise BuildError(
            f"{page.file}: placeholder {{{{{leftover.group(1)}}}}} is left in the output "
            "(a data value contains it)"
        )
    return output


# -- the build -------------------------------------------------------------


def check_target(target: str) -> str:
    """Validates a ``copy`` destination and returns it as a POSIX path."""
    path = PurePosixPath(target)
    bad = (
        "\\" in target
        or ":" in target
        or path.is_absolute()
        or any(part in ("", ".", "..") for part in target.split("/"))
    )
    if bad:
        raise BuildError(
            f"copy target '{target}' is not a relative path inside the output directory"
        )
    return path.as_posix()


def add_output(files: dict[str, bytes], rel: str, data: bytes, origin: str) -> None:
    if rel in files:
        raise BuildError(f"{origin}: output file '{rel}' is produced twice")
    files[rel] = data


def generate(root: Path, source: Path) -> SiteOutput:
    """Reads all inputs and returns the complete set of output files."""
    config_path = source / "site.json"
    config = load_config(source)
    pages, page_paths = load_pages(source)
    release = load_release(source)
    template_path = source / "templates" / "base.html"
    template = read_text(template_path)
    if "{{content}}" not in template:
        raise BuildError(f"{template_path} has no {{{{content}}}} placeholder")

    inputs = [config_path, template_path, source / "data" / "release.json", *page_paths]
    files: dict[str, bytes] = {}

    downloads = render_downloads(release, config.rules)
    for page in pages:
        text = render_page(page, template, config, release, downloads)
        add_output(files, page.file, text.encode("utf-8"), f"pages/{page.file}")

    tokens_path = root / config.tokens
    inputs.append(tokens_path)
    block = render_tokens_css(read_json(tokens_path), config.tokens)
    site_css_path = source / "static" / "css" / "site.css"
    inputs.append(site_css_path)
    css = block + "\n" + read_text(site_css_path)
    add_output(files, "css/site.css", css.encode("utf-8"), "css/site.css")

    static = source / "static"
    for path in sorted(static.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(static).as_posix()
        if rel == "css/site.css":
            continue
        inputs.append(path)
        add_output(files, rel, path.read_bytes(), f"static/{rel}")

    for entry in config.copy:
        target = check_target(entry.target)
        copy_source = root / entry.source
        if not copy_source.is_file():
            raise BuildError(f"copy source {entry.source} does not exist")
        inputs.append(copy_source)
        add_output(files, target, copy_source.read_bytes(), f"copy {entry.source}")

    sitemap = render_sitemap(config, pages)
    add_output(files, "sitemap.xml", sitemap.encode("utf-8"), "sitemap.xml")
    return SiteOutput(files, inputs, len(pages))


def check_out_dir(out: Path, protected: list[Path]) -> None:
    """Refuses an output directory that would delete the sources."""
    if out.exists() and not out.is_dir():
        raise BuildError(f"{out} exists and is not a directory")
    for path in protected:
        if out == path or out in path.parents:
            raise BuildError(f"refusing to delete {out}: it is, or contains, {path}")


def write_output(out: Path, files: Mapping[str, bytes]) -> None:
    if out.is_symlink():
        out.unlink()
    elif out.exists():
        shutil.rmtree(out)
    for rel in sorted(files):
        target = out / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(files[rel])
    out.mkdir(parents=True, exist_ok=True)


def build_site(root: Path, out: Path, source: Path | None = None) -> int:
    """Builds the site into ``out`` and returns the number of pages written."""
    root = root.resolve()
    source = (source if source is not None else root / "website").resolve()
    out = out.resolve()
    check_out_dir(out, [root, source])
    result = generate(root, source)
    check_out_dir(out, [path.resolve() for path in result.inputs])
    write_output(out, result.files)
    return result.page_count


# -- release refresh -------------------------------------------------------


def fetch_releases(repo: str) -> object:
    """Fetches the newest release from the GitHub API and returns the decoded JSON."""
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": API_USER_AGENT,
    }
    token = os.environ.get("GITHUB_TOKEN", "")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(
        f"https://api.github.com/repos/{repo}/releases?per_page=1", headers=headers
    )
    with urllib.request.urlopen(request, timeout=API_TIMEOUT_SECONDS) as response:
        return json.loads(response.read().decode("utf-8"))


def convert_release(entries: object, repo: str) -> dict[str, object]:
    """Turns the API response into the ``release.json`` shape, checking every URL."""
    prefix = f"https://github.com/{repo}/releases/"
    if not isinstance(entries, list) or not entries:
        raise BuildError(f"the GitHub API returned no releases for {repo}")
    entry = require_object(entries[0], "the first release")

    def url(value: object, where: str) -> str:
        text = require_string(value, where)
        if not text.startswith(prefix):
            raise BuildError(f"{where} '{text}' does not start with {prefix}")
        return text

    prerelease = entry.get("prerelease")
    if not isinstance(prerelease, bool):
        raise BuildError("the release has no boolean 'prerelease'")

    assets: list[dict[str, object]] = []
    for index, raw in enumerate(require_list(entry.get("assets"), "the release 'assets'")):
        asset = require_object(raw, f"assets[{index}]")
        size = asset.get("size")
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise BuildError(f"assets[{index}].size must be a non-negative integer")
        assets.append(
            {
                "name": require_string(asset.get("name"), f"assets[{index}].name"),
                "size": size,
                "url": url(asset.get("browser_download_url"), f"assets[{index}].download URL"),
            }
        )
    assets.sort(key=lambda asset: str(asset["name"]))

    return {
        "tag": require_string(entry.get("tag_name"), "the release 'tag_name'"),
        "prerelease": prerelease,
        "published_at": require_string(entry.get("published_at"), "the release 'published_at'"),
        "html_url": url(entry.get("html_url"), "the release 'html_url'"),
        "assets": assets,
    }


def refresh_release(
    root: Path,
    source: Path | None = None,
    fetch: Callable[[str], object] | None = None,
) -> Path:
    """Rewrites ``data/release.json`` from the GitHub API and returns its path."""
    source = (source if source is not None else root / "website").resolve()
    repo = load_config(source).repo
    fetch = fetch if fetch is not None else fetch_releases
    try:
        response = fetch(repo)
    except (OSError, ValueError) as error:
        raise BuildError(f"could not fetch the latest release of {repo}: {error}") from None
    data = convert_release(response, repo)

    path = source / "data" / "release.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes((json.dumps(data, indent=1) + "\n").encode("utf-8"))
    return path


# -- command line ----------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--root", default=str(REPO_ROOT), help="repository root (default: this checkout)"
    )
    parser.add_argument(
        "--out",
        default=None,
        help=f"output directory (default: {DEFAULT_OUT.as_posix()} below the root)",
    )
    parser.add_argument(
        "--refresh-release",
        action="store_true",
        help="fetch the latest release from GitHub and rewrite website/data/release.json",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    out = Path(args.out).resolve() if args.out else root / DEFAULT_OUT
    try:
        if args.refresh_release:
            refresh_release(root)
        count = build_site(root, out)
    except BuildError as error:
        print(f"site_build: {error}", file=sys.stderr)
        return 1
    print(f"site_build: wrote {count} pages to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
