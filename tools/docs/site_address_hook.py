# SPDX-License-Identifier: GPL-3.0-or-later
"""MkDocs hook: write the site's address into pages that name it.

A page writes `{{ site_url }}` where it needs the address. The value is `site_url` of mkdocs.yml, so moving the site to
another address is one edit there (tools/ci/check_site_address.py, docs/ci.md).
"""

from __future__ import annotations

TOKEN = "{{ site_url }}"


def fill(markdown: str, site_url: str) -> str:
    return markdown.replace(TOKEN, site_url.rstrip("/") + "/")


def on_page_markdown(markdown, page, config, files):  # noqa: ANN001, ANN201 (MkDocs passes its own types)
    return fill(markdown, config["site_url"] or "")
