# SPDX-License-Identifier: GPL-3.0-or-later
"""The address of the documentation site, read from the one place that holds it: `site_url` in mkdocs.yml.

The address is fixed: https://iainchesworthlabs.com/virtual-serial-studio/. The company site serves each app under a path of its own,
and the deploy role of this repository may write that path only. tools/ci/check_site_address.py fails when the address is written
down anywhere else. See docs/ci.md, "The site address".
"""

from __future__ import annotations

import re
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[2]
FIXED_ADDRESS = "https://iainchesworthlabs.com/virtual-serial-studio/"


def _value(root: Path, key: str) -> str:
    text = (root / "mkdocs.yml").read_text(encoding="utf-8")
    found = re.search(rf"^{key}:\s*(\S+)\s*$", text, re.MULTILINE)
    if not found:
        raise ValueError(f"mkdocs.yml has no {key}")
    return found.group(1).strip("'\"")


def site_url(root: Path = ROOT) -> str:
    """`site_url` of mkdocs.yml, with exactly one trailing slash."""
    value = _value(root, "site_url").rstrip("/") + "/"
    parts = urlparse(value)
    if parts.scheme != "https" or not parts.netloc:
        raise ValueError(f"site_url must be an https address, not {value!r}")
    return value


def repo_url(root: Path = ROOT) -> str:
    """`repo_url` of mkdocs.yml, without a trailing slash."""
    return _value(root, "repo_url").rstrip("/")


def host(root: Path = ROOT) -> str:
    return urlparse(site_url(root)).netloc


def path(root: Path = ROOT) -> str:
    return urlparse(site_url(root)).path
