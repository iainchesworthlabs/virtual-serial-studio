# SPDX-License-Identifier: GPL-3.0-or-later
"""MkDocs hook: the version of the program the pages describe, from the one place that holds it.

`project(virtual_serial_studio VERSION x.y.z ...)` in CMakeLists.txt is the version of the program
(tools/check_release_version.py holds a release to it). The hook puts it in `config.extra.version`, where the templates read it: every page of the site says which
version it describes. Nothing in a page's Markdown names the version, so a release does not mean editing every page.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROJECT = re.compile(r"^\s*project\(\s*virtual_serial_studio\b[^)]*?\bVERSION\s+(\d+\.\d+\.\d+)", re.MULTILINE | re.DOTALL)


def read_version(cmake_text: str) -> str:
    found = PROJECT.search(cmake_text)
    if not found:
        raise ValueError("site_version_hook: CMakeLists.txt has no project(virtual_serial_studio VERSION x.y.z)")
    return found.group(1)


def on_config(config):  # noqa: ANN001, ANN201 (MkDocs passes its own types)
    root = Path(config["docs_dir"]).resolve().parent
    config["extra"]["version"] = read_version((root / "CMakeLists.txt").read_text(encoding="utf-8"))
    return config
