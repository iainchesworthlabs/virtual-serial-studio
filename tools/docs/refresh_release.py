#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Refresh the release data of the site from GitHub (docs/ci.md, "Release data and the update feed").

    python tools/docs/refresh_release.py [--root .]

It makes one request to the GitHub API for the releases of the repository that `repo_url` of mkdocs.yml names (pre-releases
included, drafts not), and then

- rewrites tools/docs/release/release.json from the newest release: its tag, date, pre-release flag, address and files;
- downloads the `update-feed.json` file of the release with the greatest version that has one (a final release or a pre-release)
  into tools/docs/release/update-feed.json, as the bytes it is. The tool does not read, check or sign it: tools/ci/check_release_data.py
  verifies it with the keys that Studio trusts, and the site build copies it unchanged to updates/v1/feed.json.

Both files are written only after everything has been read, so a failure leaves them as they were. With no release that has a feed the
stored feed is left alone, and the tool says so.

Limits: the API answer may be 16 MiB and the feed 64 KiB, each request has 15 seconds, and only https is used. A redirect is followed
only to github.com, api.github.com and a subdomain of githubusercontent.com, which is where GitHub serves a release file from. The token
`GH_TOKEN` or `GITHUB_TOKEN`, when one is set, goes to the API as a Bearer header and nowhere else: not to a download, and not to another
host after a redirect. It is never printed.

This is the one command that uses the network. The site build, its checks and the tests do not. The standard library is all it needs.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from collections.abc import Callable
from http.client import HTTPException
from pathlib import Path
from urllib.parse import urljoin, urlsplit

HERE = Path(__file__).resolve().parent
for folder in (HERE, HERE.parent, HERE.parent / "ci"):
    if str(folder) not in sys.path:
        sys.path.insert(0, str(folder))

import site_address  # noqa: E402  (tools/ci)
import site_release_hook as hook  # noqa: E402  (this folder)
import update_feed  # noqa: E402  (tools/)

ROOT = HERE.parents[1]

USER_AGENT = "virtual-serial-studio-docs"
TIMEOUT_SECONDS = 15
PAGE_SIZE = 100
MAX_API_BYTES = 16 * 1024 * 1024
MAX_FEED_BYTES = 64 * 1024
FEED_ASSET = hook.FEED_NAME

API_HOSTS = frozenset({"api.github.com", "github.com"})
DOWNLOAD_HOST_SUFFIX = ".githubusercontent.com"

Log = Callable[[str], None]


class RefreshError(Exception):
    """The release data could not be refreshed. Nothing was changed."""


# -- the network ---------------------------------------------------------------------------------------------------------


def host_allowed(url: str) -> bool:
    """True for an https address on GitHub's own hosts."""
    parts = urlsplit(url)
    host = (parts.hostname or "").lower()
    return parts.scheme == "https" and (host in API_HOSTS or host.endswith(DOWNLOAD_HOST_SUFFIX))


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    """Follow a redirect to GitHub's hosts only, and never take the token to another host. The address of a refused redirect is not
    shown, because a download address can carry a signature in its query."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: ANN001, ANN201 (urllib's own signature)
        absolute = urljoin(req.full_url, newurl)
        if not host_allowed(absolute):
            raise urllib.error.URLError(f"a redirect to {urlsplit(absolute).hostname or 'no host'} was refused (https and GitHub's hosts only)")
        followed = super().redirect_request(req, fp, code, msg, headers, absolute)
        if followed is not None and urlsplit(absolute).hostname != urlsplit(req.full_url).hostname:
            followed.remove_header("Authorization")
        return followed


_OPENER = urllib.request.build_opener(SafeRedirect)


def open_url(request: urllib.request.Request, timeout: float):  # noqa: ANN201
    """The one place that opens a connection. The tests replace it."""
    if urlsplit(request.full_url).scheme != "https":
        raise urllib.error.URLError("only https addresses are used")
    return _OPENER.open(request, timeout=timeout)


def token_from_environment() -> str:
    return os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""


def read_limited(response: object, limit: int, what: str) -> bytes:
    data = response.read(limit + 1)  # type: ignore[attr-defined]
    if len(data) > limit:
        raise ValueError(f"{what} is larger than {limit} bytes")
    return bytes(data)


def fetch_releases(repo: str) -> object:
    """The newest releases (one page, newest first) from the GitHub API, decoded."""
    headers = {"Accept": "application/vnd.github+json", "User-Agent": USER_AGENT}
    token = token_from_environment()
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(f"https://api.github.com/repos/{repo}/releases?per_page={PAGE_SIZE}", headers=headers)
    with open_url(request, TIMEOUT_SECONDS) as response:
        return json.loads(read_limited(response, MAX_API_BYTES, "the answer of the GitHub API").decode("utf-8"))


def download_feed(url: str) -> bytes:
    """The update feed from the address of a release file. No token is sent: a release file is public. One byte more than the limit is
    read, so that a file over the limit is seen."""
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with open_url(request, TIMEOUT_SECONDS) as response:
        return bytes(response.read(MAX_FEED_BYTES + 1))


# -- what to keep --------------------------------------------------------------------------------------------------------


def published(entries: object) -> object:
    """The releases without the drafts (a draft has no tag to download from yet). Anything that is not a list is left for the
    conversion to refuse."""
    if not isinstance(entries, list):
        return entries
    return [entry for entry in entries if not (isinstance(entry, dict) and entry.get("draft") is True)]


def convert_release(entries: object, repo: str) -> dict[str, object]:
    """The record of release.json for the first published release, with every address checked to be one of `repo`'s releases."""
    try:
        return update_feed.release_record(published(entries), repo)
    except ValueError as error:
        raise RefreshError(str(error)) from None


def newest_feed(entries: object, repo: str) -> tuple[str, str] | None:
    """The tag and the download address of the update feed of the release with the greatest version that has one (final or
    pre-release, never a draft), or None. The address must be one of this repository's releases."""
    prefix = f"https://github.com/{repo}/releases/"
    if not isinstance(entries, list):
        return None
    best: tuple[str, str, str] | None = None
    for raw in entries:
        if not isinstance(raw, dict) or raw.get("draft") is True:
            continue
        tag = raw.get("tag_name")
        assets = raw.get("assets")
        if not isinstance(tag, str) or not isinstance(assets, list):
            continue
        version = tag.removeprefix("v")
        if not update_feed.is_version(version):
            continue
        for asset in assets:
            if not isinstance(asset, dict) or asset.get("name") != FEED_ASSET:
                continue
            url = asset.get("browser_download_url")
            if not isinstance(url, str) or not url.startswith(prefix):
                raise RefreshError(f"the {FEED_ASSET} of {tag} has the download address '{url}', which does not start with {prefix}")
            if best is None or update_feed.compare_versions(version, best[0]) > 0:
                best = (version, tag, url)
    return None if best is None else (best[1], best[2])


def fetch_feed(repo: str, entries: object, fetch_asset: Callable[[str], bytes], log: Log) -> bytes | None:
    """The update feed of the newest release that has one, as it is, or None when no release has one."""
    found = newest_feed(entries, repo)
    if found is None:
        log(f"no release has {FEED_ASSET}, so the stored feed is left as it is")
        return None
    tag, url = found
    try:
        data = fetch_asset(url)
    except (OSError, ValueError, HTTPException) as error:
        raise RefreshError(f"could not download {FEED_ASSET} of {tag}: {error}") from None
    if not data or len(data) > MAX_FEED_BYTES:
        raise RefreshError(f"the {FEED_ASSET} of {tag} is empty or over {MAX_FEED_BYTES} bytes")
    log(f"{FEED_ASSET} of {tag} is the update feed")
    return data


def repo_of(root: Path) -> str:
    """`owner/name` from `repo_url` of mkdocs.yml, the one place that holds it."""
    parts = urlsplit(site_address.repo_url(root))
    name = parts.path.strip("/")
    if parts.scheme != "https" or parts.netloc != "github.com" or name.count("/") != 1:
        raise RefreshError(f"repo_url of mkdocs.yml must be https://github.com/<owner>/<name>, not {site_address.repo_url(root)!r}")
    return name


def refresh(
    root: Path,
    repo: str | None = None,
    fetch: Callable[[str], object] | None = None,
    fetch_asset: Callable[[str], bytes] | None = None,
    log: Log | None = None,
) -> Path:
    """Rewrite release.json, and store the update feed when a release has one. Returns the path of release.json. Both files are
    written after everything was read. The three callables are the network, and the tests give their own."""
    repo = repo if repo is not None else repo_of(root)
    fetch = fetch if fetch is not None else fetch_releases
    fetch_asset = fetch_asset if fetch_asset is not None else download_feed
    note = log if log is not None else (lambda message: None)
    try:
        entries = fetch(repo)
    except (OSError, ValueError, HTTPException) as error:
        raise RefreshError(f"could not fetch the releases of {repo}: {error}") from None
    record = convert_release(entries, repo)
    feed = fetch_feed(repo, entries, fetch_asset, note)

    folder = root / hook.RELEASE_DIR
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / hook.RELEASE_NAME
    path.write_bytes((json.dumps(record, indent=1) + "\n").encode("utf-8"))
    if feed is not None:
        (folder / hook.FEED_NAME).write_bytes(feed)
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT, help="the repository (default: this checkout)")
    args = parser.parse_args(argv)
    try:
        path = refresh(args.root.resolve(), log=lambda message: print(f"refresh_release: {message}"))
    except (RefreshError, ValueError, OSError) as error:
        print(f"refresh_release: {error}", file=sys.stderr)
        return 1
    print(f"refresh_release: wrote {path.relative_to(args.root.resolve()).as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
