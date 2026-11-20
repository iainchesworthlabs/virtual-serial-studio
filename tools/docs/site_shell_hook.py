# SPDX-License-Identifier: GPL-3.0-or-later
"""MkDocs hook: the company shell for the pages of the /virtual-serial-studio/ section.

The shell (iainchesworthlabs.com's shared stylesheet, header and footer) is a pinned copy of the company kit in
tools/docs/site-kit/v1/, checked against the kit's own SHA256SUMS.txt by tools/ci/check_site_kit.py. Nothing is fetched at build
or run time. This hook

- lets templates include the kit's header and footer as `icl-kit/header.html` and `icl-kit/footer.html`, so the pinned
  files are used as they are and not copied a second time;
- puts `data-app="virtual-serial-studio"` on the <html> of the guide's pages (the front-page template writes it itself), so the guide
  reads the palette of docs/theme.css;
- copies the shell stylesheet to assets/icl.css of the built site.
"""

from __future__ import annotations

import shutil
from pathlib import Path

import jinja2

KIT = Path(__file__).resolve().parent / "site-kit" / "v1"
APP = "virtual-serial-studio"


def on_env(env, config, files):  # noqa: ANN001, ANN201 (MkDocs passes its own types)
    kit_loader = jinja2.PrefixLoader({"icl-kit": jinja2.FileSystemLoader(str(KIT / "kit" / "partials"))})
    env.loader = jinja2.ChoiceLoader([env.loader, kit_loader])
    return env


def add_app(html: str) -> str:
    """Add data-app to the first <html> tag. Raises when there is none, so a theme change cannot lose the palette silently."""
    if f'data-app="{APP}"' in html[:400]:
        return html
    marker = "<html"
    start = html.find(marker)
    if start < 0:
        raise ValueError("site_shell_hook: the page has no <html> tag to carry data-app")
    end = start + len(marker)
    return f'{html[:end]} data-app="{APP}"{html[end:]}'


def on_post_page(output, page, config):  # noqa: ANN001, ANN201
    return add_app(output)


def on_post_build(config):  # noqa: ANN001, ANN201
    target = Path(config["site_dir"]) / "assets"
    target.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(KIT / "icl.css", target / "icl.css")
