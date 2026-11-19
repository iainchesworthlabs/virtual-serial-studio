# SPDX-License-Identifier: GPL-3.0-or-later
"""What the site checks share: building the site, and reading a built page."""

from __future__ import annotations

import os
import subprocess
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlparse


def material_installed() -> bool:
    return subprocess.run([sys.executable, "-c", "import mkdocs, material"], capture_output=True, check=False).returncode == 0


def build(root: Path, out: Path) -> int:
    """`mkdocs build --strict` into `out`. Prints MkDocs' own output when it fails."""
    env = dict(os.environ, NO_MKDOCS_2_WARNING="true", PYTHONDONTWRITEBYTECODE="1")
    result = subprocess.run([sys.executable, "-m", "mkdocs", "build", "--strict", "--quiet", "--site-dir", str(out)],
                            cwd=root, capture_output=True, text=True, check=False, env=env)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
    return result.returncode


VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}


class Page(HTMLParser):
    """The parts of a built page the checks look at."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.title = ""
        self._in_title = False
        self.h1 = 0
        self.lang = ""
        self.canonical = ""
        self.ids: set[str] = set()
        self.refs: list[tuple[str, str, str]] = []  # (tag, attribute, value), outside the company shell
        # The links of the company shell: the kit's header and footer (<header class="icl-header">, <footer class="icl-footer">).
        # They are the company site's own links, written root-absolute as the kit asks, and are kept apart from the page's.
        self.shell_refs: list[tuple[str, str, str]] = []
        self.scripts: list[str] = []  # the type of each <script>
        self.styles = 0
        self.style_attrs = 0
        self.material = False
        self.description = ""
        self.headings: list[int] = []  # the levels of the headings of the main content, in order
        self.images: list[dict[str, str]] = []  # the attributes of each <img>
        self._main = 0  # > 0 inside <main> (the front pages) or the article of a Material page
        self._stack: list[bool] = []
        self.forms: list[dict] = []  # each: attrs, controls (tag, attrs, options, hidden), links (the hrefs inside the form)
        self.label_for: set[str] = set()  # the ids that a <label for> names
        self._form: dict | None = None
        self._open: list[tuple[str, bool, bool]] = []  # the open elements: (tag, aria-hidden, part of the company shell)

    def handle_starttag(self, tag, attrs):  # noqa: ANN001, ANN201
        a = {k: (v or "") for k, v in attrs}
        self._form_start(tag, a)
        if tag in ("main", "article") and (tag == "main" or "md-typeset" in a.get("class", "").split()):
            self._main += 1
            self._stack.append(tag)
        elif tag in ("main", "article"):
            self._stack.append("")
        if self._main and len(tag) == 2 and tag[0] == "h" and tag[1] in "123456":
            self.headings.append(int(tag[1]))
        if tag == "img":
            self.images.append(a)
        if tag == "meta" and a.get("name") == "description":
            self.description = a.get("content", "")
        if tag == "html":
            self.lang = a.get("lang", "")
        if tag == "title":
            self._in_title = True
        if tag == "h1":
            self.h1 += 1
        if tag == "script":
            self.scripts.append(a.get("type", ""))
        if tag == "style":
            self.styles += 1
        if "style" in a:
            self.style_attrs += 1
        if "id" in a:
            self.ids.add(a["id"])
        if tag == "a" and a.get("name"):
            self.ids.add(a["name"])
        if "class" in a and "md-container" in a["class"].split():
            self.material = True
        if tag == "link" and a.get("rel") == "canonical":
            self.canonical = a.get("href", "")
            return
        refs = self.shell_refs if any(shell for _, _, shell in self._open) else self.refs
        for attribute in ("href", "src"):
            if attribute in a:
                refs.append((tag, attribute, a[attribute]))
        if "srcset" in a:
            for part in a["srcset"].split(","):
                if part.strip():
                    refs.append((tag, "srcset", part.split()[0]))

    def _form_start(self, tag: str, a: dict[str, str]) -> None:
        """Forms, their controls and their labels. An element hidden with aria-hidden hides what is inside it."""
        hidden = any(h for _, h, _ in self._open) or a.get("aria-hidden") == "true"
        if tag == "form":
            self._form = {"attrs": a, "controls": [], "links": []}
            self.forms.append(self._form)
        elif tag in ("input", "select", "textarea", "button") and self._form is not None:
            self._form["controls"].append({"tag": tag, "attrs": a, "options": [], "hidden": hidden})
        elif tag == "option" and self._form is not None and self._form["controls"] and self._form["controls"][-1]["tag"] == "select":
            self._form["controls"][-1]["options"].append(a.get("value", ""))
        elif tag == "a" and self._form is not None:
            self._form["links"].append(a.get("href", ""))
        elif tag == "label" and "for" in a:
            self.label_for.add(a["for"])
        if tag not in VOID:
            classes = a.get("class", "").split()
            shell = (tag == "header" and "icl-header" in classes) or (tag == "footer" and "icl-footer" in classes)
            self._open.append((tag, a.get("aria-hidden") == "true", shell))

    def handle_endtag(self, tag):  # noqa: ANN001, ANN201
        if tag == "form":
            self._form = None
        while self._open and self._open[-1][0] != tag and any(t == tag for t, _, _ in self._open):
            self._open.pop()  # an element whose end tag is optional (p, li, option) was left open
        if self._open and self._open[-1][0] == tag:
            self._open.pop()
        if tag == "title":
            self._in_title = False
        if tag in ("main", "article") and self._stack:
            if self._stack.pop():
                self._main -= 1

    def handle_data(self, data):  # noqa: ANN001, ANN201
        if self._in_title:
            self.title += data


def parse(path: Path) -> Page:
    page = Page()
    page.feed(path.read_text(encoding="utf-8"))
    return page


def target_file(site: Path, page_file: Path, link: str) -> Path | None:
    """The file a relative link names, or None when it leaves the site."""
    path = unquote(urlparse(link).path)
    page_file = page_file.resolve()
    target = (page_file.parent / path).resolve() if path else page_file
    try:
        target.relative_to(site.resolve())
    except ValueError:
        return None
    if target.is_dir():
        target = target / "index.html"
    return target
