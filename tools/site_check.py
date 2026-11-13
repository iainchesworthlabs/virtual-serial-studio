#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check a built static website directory.

The checker is run in CI on the output of the site build.  It reads every
``*.html`` page and every ``*.css`` file under the directory and reports:

* links: broken relative links and fragments, absolute-path links, forbidden
  schemes, hosts that are not on the allow-list and pages nobody links to;
* resources: anything that would make the browser fetch from another host,
  scripts, frames, plug-ins, event-handler attributes and meta refreshes;
* accessibility: the subset of WCAG 2.2 AA that can be checked statically
  (language, title, headings, landmarks, names, tables, ids, forms and the
  accessibility-related CSS rules);
* contrast: the colour pairs named in the configuration, for the light and
  the dark token sets.

Every problem is one line, ``FILE: RULE-ID: message``, with FILE relative to
the site directory.  The tool uses only the Python standard library and makes
no network request unless ``--external`` is given.

Usage
-----
    python3 tools/site_check.py [DIR] [--config website/site.json]
                                [--tokens PATH] [--external]

Exit status: 0 when every check passes, 1 when there are problems, 2 when the
directory is missing or holds no page.

Configuration keys (all optional): ``external_paths``, ``allowed_link_hosts``,
``contrast_pairs``, ``draft_pages`` and ``tokens``.
"""

from __future__ import annotations

import argparse
import json
import posixpath
import re
import sys
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field
from html.parser import HTMLParser
from http.client import HTTPException
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlsplit

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SITE_DIR = REPO_ROOT / "build" / "website"
DEFAULT_CONFIG = REPO_ROOT / "website" / "site.json"
DEFAULT_TOKENS = "apps/vss_gui/src/theme/resources/tokens.json"
FETCH_TIMEOUT = 15.0
SITE_CSS = "css/site.css"

# FILE, RULE-ID, message.
Problem = tuple[str, str, str]
Fetch = Callable[[str], int]

VOID_TAGS = frozenset(
    "area base br col embed hr img input link meta param source track wbr".split()
)
# Elements whose end tag HTML allows to be left out.
OPTIONAL_END = frozenset(
    "html head body p li dt dd tr td th thead tbody tfoot option optgroup colgroup".split()
)
HEADING_TAGS = frozenset({"h1", "h2", "h3", "h4", "h5", "h6"})
INVISIBLE_TAGS = frozenset({"head", "title", "script", "style", "template"})
FORBIDDEN_TAGS = frozenset({"script", "iframe", "object", "embed"})
NAV_TAGS = frozenset({"a", "area"})
URL_ATTRIBUTES = ("href", "xlink:href", "src", "poster", "action", "formaction")
ARIA_REFERENCES = ("aria-labelledby", "aria-describedby", "aria-controls")
EXEMPT_INPUT_TYPES = frozenset({"hidden", "submit", "button", "reset", "image"})
VAGUE_NAMES = frozenset({"click here", "here", "more", "read more", "link"})
NAME_PUNCTUATION = " .…:!>»›→-"
# Closing an element of the first kind implies closing the listed kinds,
# searching no further than the scope stoppers.
IMPLIED_END = {
    "li": ("li",),
    "dt": ("dt", "dd"),
    "dd": ("dt", "dd"),
    "td": ("td", "th"),
    "th": ("td", "th"),
    "option": ("option",),
}
SCOPE_STOPPERS = frozenset(
    "ul ol menu dl table tbody thead tfoot tr select datalist optgroup".split()
)

SCHEME_RE = re.compile(r"^([a-zA-Z][a-zA-Z0-9+.-]*):")
COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
IMPORT_RE = re.compile(r"@import\b[^;{}]*;?", re.IGNORECASE)
URL_RE = re.compile(
    r"""url\(\s*(?:"([^"]*)"|'([^']*)'|([^)\s]*))\s*\)""", re.IGNORECASE
)
OUTLINE_RE = re.compile(
    r"outline\s*:\s*(?:none|0(?:px)?)\s*(?:!\s*important\s*)?(?:;|\}|$)",
    re.IGNORECASE,
)
FONT_SIZE_RE = re.compile(r"(?<![\w-])font-size\s*:\s*([^;}]*)", re.IGNORECASE)
PX_RE = re.compile(r"[\d.]\s*px\b", re.IGNORECASE)
MOTION_RE = re.compile(
    r"@keyframes|(?<![\w-])(?:animation|transition)(?:-[a-z-]+)?\s*:", re.IGNORECASE
)
HEX_RE = re.compile(
    r"#(?:[0-9a-fA-F]{8}|[0-9a-fA-F]{6}|[0-9a-fA-F]{4}|[0-9a-fA-F]{3})(?![0-9a-zA-Z_-])"
)
COLOUR_FUNCTION_RE = re.compile(r"(?<![\w-])(?:rgb|rgba|hsl|hsla)\s*\(", re.IGNORECASE)
ROOT_BLOCK_RE = re.compile(r":root\s*\{")
DARK_BLOCK_RE = re.compile(
    r"@media\s*\(\s*prefers-color-scheme\s*:\s*dark\s*\)\s*\{", re.IGNORECASE
)


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------


class Elem:
    """One open or closed element, with the text collected while it was open."""

    __slots__ = (
        "tag", "attrs", "hidden", "in_svg", "text", "titles",
        "has_caption", "th_total", "th_bare",
    )

    def __init__(self, tag: str, attrs: dict[str, str], hidden: bool, in_svg: bool):
        self.tag = tag
        self.attrs = attrs
        self.hidden = hidden
        self.in_svg = in_svg
        self.text: list[str] = []
        self.titles: list[Elem] = []
        self.has_caption = False
        self.th_total = 0
        self.th_bare = 0

    def plain_text(self) -> str:
        return normalise(" ".join(self.text))


@dataclass
class Ref:
    """A URL found in an attribute."""

    tag: str
    attr: str
    value: str
    rel: str = ""


@dataclass
class Page:
    """What the checks need to know about one HTML page."""

    html_attrs: dict[str, str] | None = None
    title_elem: Elem | None = None
    meta_description: str | None = None
    meta_viewport: str | None = None
    headings: list[Elem] = field(default_factory=list)
    mains: list[Elem] = field(default_factory=list)
    navs: list[Elem] = field(default_factory=list)
    headers: int = 0
    footers: int = 0
    anchors: list[Elem] = field(default_factory=list)
    buttons: list[Elem] = field(default_factory=list)
    imgs: list[Elem] = field(default_factory=list)
    svgs: list[Elem] = field(default_factory=list)
    role_imgs: list[Elem] = field(default_factory=list)
    tables: list[Elem] = field(default_factory=list)
    controls: list[tuple[Elem, bool]] = field(default_factory=list)
    ids: list[str] = field(default_factory=list)
    anchor_names: set[str] = field(default_factory=set)
    aria_refs: list[tuple[str, str]] = field(default_factory=list)
    label_for: set[str] = field(default_factory=set)
    tabindexes: list[str] = field(default_factory=list)
    forbidden: list[str] = field(default_factory=list)
    refs: list[Ref] = field(default_factory=list)
    style_blocks: list[str] = field(default_factory=list)
    style_attrs: list[str] = field(default_factory=list)
    body_text: list[str] = field(default_factory=list)
    parse_problems: list[str] = field(default_factory=list)

    def id_set(self) -> set[str]:
        return set(self.ids) | self.anchor_names

    def title(self) -> str | None:
        if self.title_elem is None:
            return None
        return self.title_elem.plain_text()


def normalise(text: str) -> str:
    """Collapse runs of white space and trim."""
    return " ".join(text.split())


class _Parser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.page = Page()
        self.stack: list[Elem] = []

    # -- tokenizer callbacks ------------------------------------------------

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = {key: (value if value is not None else "") for key, value in attrs}
        self._close_implied(tag)
        parent = self.stack[-1] if self.stack else None
        hidden = bool(parent and parent.hidden) or (
            values.get("aria-hidden", "").strip().lower() == "true"
        )
        in_svg = bool(parent and parent.in_svg) or tag == "svg"
        elem = Elem(tag, values, hidden, in_svg)
        self._record(elem)
        if tag not in VOID_TAGS:
            self.stack.append(elem)

    def handle_startendtag(
        self, tag: str, attrs: list[tuple[str, str | None]]
    ) -> None:
        self.handle_starttag(tag, attrs)
        self.handle_endtag(tag)

    def handle_endtag(self, tag: str) -> None:
        if tag in VOID_TAGS:
            return
        for index in range(len(self.stack) - 1, -1, -1):
            if self.stack[index].tag == tag:
                break
        else:
            self._problem(f"unexpected closing </{tag}>")
            return
        for elem in self.stack[index + 1:]:
            if elem.tag not in OPTIONAL_END:
                self._problem(f"<{elem.tag}> is not closed before </{tag}>")
        del self.stack[index:]

    def handle_data(self, data: str) -> None:
        if not self.stack:
            return
        top = self.stack[-1]
        if top.tag == "script":
            return
        if top.tag == "style":
            self.page.style_blocks.append(data)
            return
        if not top.hidden:
            for elem in self.stack:
                elem.text.append(data)
        if not any(elem.tag in INVISIBLE_TAGS for elem in self.stack):
            self.page.body_text.append(data)

    def finish(self) -> None:
        self.close()
        for elem in self.stack:
            if elem.tag not in OPTIONAL_END:
                self._problem(f"<{elem.tag}> is never closed")
        self.stack.clear()

    # -- helpers --------------------------------------------------------------

    def _problem(self, message: str) -> None:
        self.page.parse_problems.append(f"{message} (line {self.getpos()[0]})")

    def _nearest(self, tag: str) -> Elem | None:
        for elem in reversed(self.stack):
            if elem.tag == tag:
                return elem
        return None

    def _close_implied(self, tag: str) -> None:
        if tag == "tr":
            while self.stack and self.stack[-1].tag in ("td", "th", "tr"):
                self.stack.pop()
            return
        closes = IMPLIED_END.get(tag)
        if closes is None:
            return
        for index in range(len(self.stack) - 1, -1, -1):
            name = self.stack[index].tag
            if name in closes:
                del self.stack[index:]
                return
            if name in SCOPE_STOPPERS:
                return

    def _record(self, elem: Elem) -> None:
        page = self.page
        tag, attrs = elem.tag, elem.attrs
        if tag == "html":
            if page.html_attrs is None:
                page.html_attrs = attrs
        elif tag == "title":
            if elem.in_svg:
                if self.stack and self.stack[-1].tag == "svg":
                    self.stack[-1].titles.append(elem)
            elif page.title_elem is None:
                page.title_elem = elem
        elif tag == "meta":
            self._record_meta(attrs)
        elif tag in HEADING_TAGS:
            page.headings.append(elem)
        elif tag == "main":
            page.mains.append(elem)
        elif tag == "header":
            page.headers += 1
        elif tag == "footer":
            page.footers += 1
        elif tag == "nav":
            page.navs.append(elem)
        elif tag == "a":
            page.anchors.append(elem)
            if attrs.get("name"):
                page.anchor_names.add(attrs["name"])
        elif tag in ("button", "summary"):
            page.buttons.append(elem)
        elif tag == "img":
            page.imgs.append(elem)
            alt = attrs.get("alt", "").strip()
            if alt and not elem.hidden:
                for open_elem in self.stack:
                    open_elem.text.append(alt)
        elif tag == "svg":
            page.svgs.append(elem)
        elif tag == "table":
            page.tables.append(elem)
        elif tag == "caption":
            table = self._nearest("table")
            if table is not None:
                table.has_caption = True
        elif tag == "th":
            table = self._nearest("table")
            if table is not None:
                table.th_total += 1
                if not attrs.get("scope", "").strip():
                    table.th_bare += 1
        elif tag in ("input", "select", "textarea"):
            inside_label = self._nearest("label") is not None
            page.controls.append((elem, inside_label))
        elif tag == "label":
            if "for" in attrs:
                page.label_for.add(attrs["for"])
                page.aria_refs.append(("label for", attrs["for"]))
        if tag in FORBIDDEN_TAGS:
            page.forbidden.append(f"<{tag}> is not allowed")
        self._record_generic(elem)

    def _record_meta(self, attrs: dict[str, str]) -> None:
        page = self.page
        name = attrs.get("name", "").strip().lower()
        if name == "description" and page.meta_description is None:
            page.meta_description = attrs.get("content", "")
        elif name == "viewport" and page.meta_viewport is None:
            page.meta_viewport = attrs.get("content", "")
        if attrs.get("http-equiv", "").strip().lower() == "refresh":
            page.forbidden.append('<meta http-equiv="refresh"> is not allowed')

    def _record_generic(self, elem: Elem) -> None:
        page = self.page
        tag, attrs = elem.tag, elem.attrs
        if attrs.get("id", "").strip():
            page.ids.append(attrs["id"].strip())
        for attr in ARIA_REFERENCES:
            for token in attrs.get(attr, "").split():
                page.aria_refs.append((attr, token))
        if "tabindex" in attrs:
            page.tabindexes.append(attrs["tabindex"])
        if attrs.get("role", "").strip().lower() == "img" and tag != "svg":
            page.role_imgs.append(elem)
        for key in attrs:
            if key.startswith("on") and key.isalpha() and len(key) > 2:
                page.forbidden.append(f"event handler attribute {key} is not allowed")
        if "style" in attrs:
            page.style_attrs.append(attrs["style"])
        rel = attrs.get("rel", "").lower()
        for attr in URL_ATTRIBUTES:
            if attr in attrs:
                page.refs.append(Ref(tag, attr, attrs[attr], rel))
        if tag == "object" and "data" in attrs:
            page.refs.append(Ref(tag, "data", attrs["data"], rel))
        for attr in ("srcset", "imagesrcset"):
            if attr in attrs and not attrs[attr].strip().startswith("data:"):
                for candidate in attrs[attr].split(","):
                    parts = candidate.split()
                    if parts:
                        page.refs.append(Ref(tag, attr, parts[0], rel))


def parse_page(html: str) -> Page:
    """Parse one page; malformed markup is recorded, never raised."""
    parser = _Parser()
    try:
        parser.feed(html)
        parser.finish()
    except Exception as error:  # noqa: BLE001 - the parser must never stop the run
        parser.page.parse_problems.append(f"cannot parse the page: {error}")
    return parser.page


# ---------------------------------------------------------------------------
# Links and resources
# ---------------------------------------------------------------------------


@dataclass
class Site:
    """The built site as the link checks see it."""

    files: set[str]
    pages: dict[str, Page]
    external_paths: tuple[str, ...] = ()
    allowed_hosts: frozenset[str] | None = None


@dataclass
class RefResult:
    problems: list[Problem] = field(default_factory=list)
    internal: int = 0
    external: set[str] = field(default_factory=set)
    targets: set[str] = field(default_factory=set)


def classify(value: str) -> str:
    """Return fragment, protocol-relative, absolute, scheme or relative."""
    if value.startswith("#"):
        return "fragment"
    if value.startswith("//"):
        return "protocol-relative"
    if value.startswith("/"):
        return "absolute"
    if SCHEME_RE.match(value):
        return "scheme"
    return "relative"


def resolve_relative(source: str, url: str) -> str | None:
    """Return the site-relative path ``url`` names from file ``source``.

    The result is ``""`` for the site root and None when the path leaves the
    site.  Query and fragment are dropped and percent escapes are decoded.
    """
    path = unquote(url.split("#", 1)[0].split("?", 1)[0])
    joined = posixpath.join(posixpath.dirname(source), path) if path else source
    result = posixpath.normpath(joined)
    if result == "..":
        return None
    if result.startswith("../"):
        return None
    return "" if result == "." else result


def target_file(files: set[str], url: str, resolved: str) -> tuple[str, str]:
    """Return (status, file) for a resolved path: ok, missing or no-index."""
    want_directory = url.split("#", 1)[0].split("?", 1)[0].endswith("/")
    if not want_directory and resolved in files:
        return "ok", resolved
    prefix = f"{resolved}/" if resolved else ""
    if resolved == "" or any(name.startswith(prefix) for name in files):
        index = f"{prefix}index.html"
        if index in files:
            return "ok", index
        return "no-index", resolved
    return "missing", resolved


def has_prefix(value: str, prefixes: tuple[str, ...]) -> bool:
    trimmed = value
    while trimmed.startswith("./"):
        trimmed = trimmed[2:]
    return any(trimmed.startswith(prefix) for prefix in prefixes if prefix)


def link_target(source: str, href: str) -> str:
    """Normalise a link so equal targets compare equal."""
    value = href.strip()
    if value.startswith("#"):
        return f"{source}{value}"
    if classify(value) != "relative":
        return value
    resolved = resolve_relative(source, value)
    if resolved is None:
        return value
    fragment = value.split("#", 1)[1] if "#" in value else ""
    return resolved + (f"#{fragment}" if fragment else "")


def _describe(ref: Ref) -> str:
    return f'<{ref.tag} {ref.attr}="{ref.value.strip()}">'


def check_fragment(
    name: str, ref: Ref, fragment: str, target: str, site: Site
) -> list[Problem]:
    """Check ``fragment`` against the ids of page ``target``."""
    wanted = unquote(fragment)
    if wanted in ("", "top") or wanted.startswith(":~:"):
        return []
    page = site.pages.get(target)
    if page is None or wanted in page.id_set():
        return []
    return [(name, "link-fragment", f'{_describe(ref)} has no id "{wanted}" in {target}')]


def check_refs(name: str, page: Page, site: Site) -> RefResult:
    """Check every URL attribute of a page."""
    result = RefResult()
    for ref in page.refs:
        value = ref.value.strip()
        if not value or (ref.tag == "link" and "canonical" in ref.rel.split()):
            continue
        if ref.tag in NAV_TAGS and ref.attr == "href":
            _check_navigation(name, ref, value, site, result)
        else:
            _check_resource(name, ref, value, site, result)
    return result


def _check_relative(
    name: str, ref: Ref, value: str, site: Site, result: RefResult, fragments: bool
) -> None:
    resolved = resolve_relative(name, value)
    if resolved is None:
        result.problems.append(
            (name, "link-broken", f"{_describe(ref)} resolves outside the site")
        )
        return
    status, target = target_file(site.files, value, resolved)
    if status == "missing":
        result.problems.append((name, "link-broken", f"{_describe(ref)} does not exist"))
        return
    if status == "no-index":
        result.problems.append(
            (name, "link-broken", f"{_describe(ref)} names a directory without index.html")
        )
        return
    if target.endswith(".html") and target != name and ref.tag in NAV_TAGS:
        result.targets.add(target)
    if fragments and "#" in value:
        result.problems.extend(
            check_fragment(name, ref, value.split("#", 1)[1], target, site)
        )


def _check_navigation(
    name: str, ref: Ref, value: str, site: Site, result: RefResult
) -> None:
    kind = classify(value)
    if kind == "fragment":
        result.internal += 1
        result.problems.extend(check_fragment(name, ref, value[1:], name, site))
    elif kind == "protocol-relative":
        result.problems.append(
            (name, "link-scheme", f"{_describe(ref)} is protocol-relative; use https:")
        )
    elif kind == "absolute":
        result.problems.append(
            (name, "link-absolute", f"{_describe(ref)} starts with /; use a relative path")
        )
    elif kind == "scheme":
        matched = SCHEME_RE.match(value)
        scheme = matched.group(1).lower() if matched else ""
        if scheme == "https":
            _check_https(name, ref, value, site, result)
        elif scheme != "mailto":
            result.problems.append(
                (name, "link-scheme",
                 f"{_describe(ref)} uses {scheme}:; only https: and mailto: are allowed")
            )
    else:
        result.internal += 1
        if has_prefix(value, site.external_paths):
            return
        _check_relative(name, ref, value, site, result, fragments=True)


def _check_https(
    name: str, ref: Ref, value: str, site: Site, result: RefResult
) -> None:
    result.external.add(value)
    host = (urlsplit(value).hostname or "").lower()
    if site.allowed_hosts is not None and host not in site.allowed_hosts:
        result.problems.append(
            (name, "link-host", f'{_describe(ref)} points to "{host}", which is not allowed')
        )


def _check_resource(
    name: str, ref: Ref, value: str, site: Site, result: RefResult
) -> None:
    kind = classify(value)
    if kind == "fragment":
        return
    if kind == "protocol-relative":
        result.problems.append(
            (name, "external-resource", f"{_describe(ref)} loads from another host")
        )
    elif kind == "absolute":
        result.problems.append(
            (name, "link-absolute", f"{_describe(ref)} starts with /; use a relative path")
        )
    elif kind == "scheme":
        if not value.lower().startswith("data:"):
            result.problems.append(
                (name, "external-resource", f"{_describe(ref)} loads from another host")
            )
    elif ref.attr not in ("action", "formaction") and not has_prefix(
        value, site.external_paths
    ):
        _check_relative(name, ref, value, site, result, fragments=False)


def check_orphans(pages: dict[str, Page], targets: dict[str, set[str]]) -> list[Problem]:
    """Report pages that no other page links to."""
    linked: set[str] = set()
    for source, found in targets.items():
        linked.update(target for target in found if target != source)
    exempt = {"index.html", "404.html"}
    return [
        (name, "orphan", "no other page links to this page")
        for name in sorted(pages)
        if name not in exempt and name not in linked
    ]


def check_policy(name: str, page: Page, files: set[str]) -> list[Problem]:
    """Report scripts, frames, handlers and fetches from inline CSS."""
    problems: list[Problem] = [(name, "no-script", msg) for msg in dict.fromkeys(page.forbidden)]
    for text in [*page.style_blocks, *page.style_attrs]:
        for rule, message in css_url_problems(text, name, files):
            problems.append((name, rule, message))
    return problems


def check_external(
    urls_by_page: dict[str, set[str]], fetch: Fetch
) -> list[Problem]:
    """Fetch each distinct https link once and report failures."""
    verdicts: dict[str, str | None] = {}
    problems: list[Problem] = []
    for name in sorted(urls_by_page):
        for url in sorted(urls_by_page[name]):
            key = url.split("#", 1)[0]
            if key not in verdicts:
                try:
                    status = fetch(key)
                except (OSError, ValueError, HTTPException) as error:
                    verdicts[key] = f"request failed: {error}"
                else:
                    verdicts[key] = f"returned status {status}" if status >= 400 else None
            if verdicts[key] is not None:
                problems.append((name, "link-external", f"{url} {verdicts[key]}"))
    return problems


def fetch_url(url: str, timeout: float = FETCH_TIMEOUT) -> int:
    """Return the HTTP status of ``url``: HEAD first, then GET.

    Raises OSError when the server cannot be reached.
    """
    headers = {"User-Agent": "site_check/1.0 (link check)"}
    status = 0
    for method in ("HEAD", "GET"):
        request = urllib.request.Request(url, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                status = response.status
        except urllib.error.HTTPError as error:
            status = error.code
        except OSError:
            if method == "GET":
                raise
            continue
        if status < 400:
            return status
    return status


# ---------------------------------------------------------------------------
# Accessibility
# ---------------------------------------------------------------------------


def _label_key(elem: Elem) -> tuple[str, str] | None:
    label = elem.attrs.get("aria-label", "").strip()
    if label:
        return "label", normalise(label).casefold()
    labelledby = normalise(elem.attrs.get("aria-labelledby", ""))
    if labelledby:
        return "ref", labelledby
    return None


def accessible_name(elem: Elem) -> tuple[str, str]:
    """Return (kind, name); kind is label, ref or text and name may be empty."""
    label = elem.attrs.get("aria-label", "").strip()
    if label:
        return "label", normalise(label)
    if elem.attrs.get("aria-labelledby", "").strip():
        return "ref", ""
    return "text", elem.plain_text()


def viewport_problems(content: str) -> list[str]:
    items: dict[str, str] = {}
    for part in re.split(r"[,;]", content):
        if "=" in part:
            key, value = part.split("=", 1)
            items[key.strip().lower()] = value.strip().lower()
    found = []
    if items.get("user-scalable") in ("no", "0"):
        found.append("user-scalable=no stops people zooming")
    try:
        if float(items["maximum-scale"]) < 2:
            found.append("maximum-scale below 2 stops people zooming")
    except (KeyError, ValueError):
        pass
    return found


def check_accessibility(name: str, page: Page) -> list[Problem]:
    """Static WCAG 2.2 AA checks for one page."""
    out: list[Problem] = []

    def add(rule: str, message: str) -> None:
        out.append((name, rule, message))

    _check_head(page, add)
    _check_headings(page, add)
    _check_landmarks(page, add)
    _check_images(page, add)
    _check_names(name, page, add)
    _check_tables(page, add)
    _check_ids(page, add)
    return out


def _check_head(page: Page, add: Callable[[str, str], None]) -> None:
    if page.html_attrs is None or not page.html_attrs.get("lang", "").strip():
        add("lang", "<html> has no lang attribute")
    title = page.title()
    if title is None:
        add("title", "the page has no <title>")
    elif not title:
        add("title", "the <title> is empty")
    if page.meta_description is None or not page.meta_description.strip():
        add("description", 'missing or empty <meta name="description">')
    if page.meta_viewport is None:
        add("viewport", 'missing <meta name="viewport">')
    else:
        for message in viewport_problems(page.meta_viewport):
            add("viewport", message)


def _check_headings(page: Page, add: Callable[[str, str], None]) -> None:
    levels = [int(elem.tag[1]) for elem in page.headings]
    if levels.count(1) != 1:
        add("h1", f"expected exactly one <h1>, found {levels.count(1)}")
    for previous, current in zip(levels, levels[1:]):
        if current > previous + 1:
            add("heading-order", f"<h{previous}> is followed by <h{current}>")
    for elem in page.headings:
        if not elem.plain_text():
            add("heading-empty", f"an empty <{elem.tag}>")


def _check_landmarks(page: Page, add: Callable[[str, str], None]) -> None:
    if len(page.mains) != 1:
        add("landmark", f"expected exactly one <main>, found {len(page.mains)}")
    elif not page.mains[0].attrs.get("id", "").strip():
        add("landmark", "<main> has no id")
    for tag, count in (("header", page.headers), ("nav", len(page.navs)),
                       ("footer", page.footers)):
        if count == 0:
            add("landmark", f"no <{tag}>")
    if len(page.navs) > 1:
        seen: dict[tuple[str, str], int] = {}
        for nav in page.navs:
            key = _label_key(nav)
            if key is None:
                add("landmark", "with several <nav> elements each needs aria-label")
            else:
                seen[key] = seen.get(key, 0) + 1
        for key, count in seen.items():
            if count > 1:
                add("landmark", f'{count} <nav> elements share the label "{key[1]}"')
    main_id = page.mains[0].attrs.get("id", "").strip() if len(page.mains) == 1 else ""
    if not page.anchors:
        add("skip-link", "the page has no skip link")
    elif main_id and page.anchors[0].attrs.get("href", "").strip() != f"#{main_id}":
        add("skip-link", f"the first link must be a skip link to #{main_id}")


def _check_images(page: Page, add: Callable[[str, str], None]) -> None:
    for img in page.imgs:
        if "alt" not in img.attrs:
            add("img-alt", f'<img src="{img.attrs.get("src", "")}"> has no alt attribute')
    for svg in page.svgs:
        attrs = svg.attrs
        if svg.hidden:
            continue
        has_label = bool(
            attrs.get("aria-label", "").strip() or attrs.get("aria-labelledby", "").strip()
        )
        has_title = any(title.plain_text() for title in svg.titles)
        if not (has_label or has_title):
            add("img-alt", '<svg> needs aria-hidden="true", or role="img" and a name')
    for elem in page.role_imgs:
        if not _label_key(elem):
            add("img-alt", f'<{elem.tag} role="img"> needs aria-label or aria-labelledby')


def _check_names(name: str, page: Page, add: Callable[[str, str], None]) -> None:
    targets: dict[str, dict[str, str]] = {}
    for anchor in page.anchors:
        if "href" not in anchor.attrs or anchor.hidden:
            continue
        kind, label = accessible_name(anchor)
        if kind == "text" and not label:
            add("link-name", f'<a href="{anchor.attrs["href"]}"> has no accessible name')
            continue
        if kind == "ref":
            continue
        if label.casefold().strip(NAME_PUNCTUATION) in VAGUE_NAMES:
            add("link-vague", f'the link name "{label}" does not say where it goes')
        targets.setdefault(label.casefold(), {})[link_target(name, anchor.attrs["href"])] = label
    for key, found in sorted(targets.items()):
        if len(found) > 1:
            where = ", ".join(sorted(found))
            add("link-dup-name", f'links named "{next(iter(found.values()))}" go to {where}')
    for button in page.buttons:
        kind, label = accessible_name(button)
        if kind == "text" and not label and not button.hidden:
            add("button-name", f"a <{button.tag}> has no accessible name")


def _check_tables(page: Page, add: Callable[[str, str], None]) -> None:
    for table in page.tables:
        if table.attrs.get("role", "").strip().lower() in ("presentation", "none"):
            continue
        if not (table.has_caption or _label_key(table)):
            add("table", "a <table> needs a <caption> or aria-label")
        if table.th_total == 0:
            add("table", "a <table> has no <th> header cell")
        if table.th_bare:
            add("table", f"{table.th_bare} <th> cell(s) have no scope")


def _check_ids(page: Page, add: Callable[[str, str], None]) -> None:
    counts: dict[str, int] = {}
    for found in page.ids:
        counts[found] = counts.get(found, 0) + 1
    for found, count in sorted(counts.items()):
        if count > 1:
            add("id-dup", f'id "{found}" is used {count} times')
    known = page.id_set()
    for attr, target in page.aria_refs:
        if target not in known:
            add("aria-ref", f'{attr} refers to "{target}", which does not exist')
    for value in page.tabindexes:
        try:
            positive = int(value.strip()) > 0
        except ValueError:
            continue
        if positive:
            add("tabindex", f'tabindex="{value.strip()}" breaks the focus order')
    for control, inside_label in page.controls:
        attrs = control.attrs
        input_type = attrs.get("type", "text").strip().lower()
        if control.tag == "input" and input_type in EXEMPT_INPUT_TYPES:
            continue
        control_id = attrs.get("id", "").strip()
        labelled = (
            inside_label
            or (control_id and control_id in page.label_for)
            or attrs.get("aria-label", "").strip()
            or attrs.get("aria-labelledby", "").strip()
        )
        if not labelled:
            add("form-label", f"a <{control.tag}> has no label")


# ---------------------------------------------------------------------------
# CSS
# ---------------------------------------------------------------------------


def strip_comments(css: str) -> str:
    return COMMENT_RE.sub(" ", css)


def css_url_problems(css: str, source: str, files: set[str]) -> list[tuple[str, str]]:
    """Report @import, off-site url() and unresolvable relative url()."""
    text = strip_comments(css)
    found: list[tuple[str, str]] = []
    for match in IMPORT_RE.finditer(text):
        statement = normalise(match.group(0))[:60]
        found.append(("external-resource", f"@import is not allowed: {statement}"))
    text = IMPORT_RE.sub(" ", text)
    for match in URL_RE.finditer(text):
        value = next((group for group in match.groups() if group is not None), "").strip()
        if not value or value.startswith("#") or "var(" in value:
            continue
        kind = classify(value)
        shown = f'url("{value}")'
        if kind == "scheme" and value.lower().startswith("data:"):
            continue
        if kind in ("protocol-relative", "scheme"):
            found.append(("external-resource", f"{shown} loads from another host"))
        elif kind == "absolute":
            found.append(("link-absolute", f"{shown} starts with /; use a relative path"))
        else:
            resolved = resolve_relative(source, value)
            if resolved is None:
                found.append(("link-broken", f"{shown} resolves outside the site"))
            elif target_file(files, value, resolved)[0] != "ok":
                found.append(("link-broken", f"{shown} does not exist"))
    return found


def _find_block(text: str, header: re.Pattern[str]) -> tuple[int, int] | None:
    """Return the span of the first block opened by ``header``, braces matched."""
    match = header.search(text)
    if match is None:
        return None
    depth = 0
    for index in range(match.end() - 1, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return match.start(), index + 1
    return match.start(), len(text)


def literal_colour_problems(css: str) -> list[str]:
    """Hex colours and rgb()/hsl() outside the generated token blocks."""
    text = strip_comments(css)
    spans = [
        span
        for span in (_find_block(text, ROOT_BLOCK_RE), _find_block(text, DARK_BLOCK_RE))
        if span is not None
    ]
    chars = list(text)
    for start, end in spans:
        chars[start:end] = " " * (end - start)
    text = URL_RE.sub(" ", "".join(chars))
    found: list[str] = []
    for match in HEX_RE.finditer(text):
        following = re.search(r"[{};]", text[match.end():])
        if following is not None and following.group() == "{":
            continue  # an id selector such as #add, not a colour
        found.append(f"literal colour {match.group(0)}; use a token variable")
    for match in COLOUR_FUNCTION_RE.finditer(text):
        found.append(f"literal colour function {match.group(0).strip()}; use a token variable")
    return found


def check_css(css_files: dict[str, str], files: set[str]) -> list[Problem]:
    """Accessibility and policy checks across the CSS files."""
    if not css_files:
        return []
    problems: list[Problem] = []
    names = sorted(css_files)
    cleaned = {name: strip_comments(css_files[name]) for name in names}
    if not any(":focus-visible" in text for text in cleaned.values()):
        problems.append((names[0], "css-focus", "no :focus-visible rule in the CSS"))
    for name in names:
        text = cleaned[name]
        if OUTLINE_RE.search(text):
            problems.append((name, "css-focus", "outline is removed; keep a visible focus ring"))
        for match in FONT_SIZE_RE.finditer(text):
            if PX_RE.search(match.group(1)):
                problems.append(
                    (name, "css-px-font",
                     f"font-size:{normalise(match.group(1))} is in px; use rem or em")
                )
        for rule, message in css_url_problems(css_files[name], name, files):
            problems.append((name, rule, message))
    users = [name for name in names if MOTION_RE.search(cleaned[name])]
    if users and not any("prefers-reduced-motion" in text for text in cleaned.values()):
        problems.append(
            (users[0], "css-motion", "animation or transition without prefers-reduced-motion")
        )
    if SITE_CSS in css_files:
        for message in literal_colour_problems(css_files[SITE_CSS]):
            problems.append((SITE_CSS, "css-literal-colour", message))
    return problems


# ---------------------------------------------------------------------------
# Colour contrast
# ---------------------------------------------------------------------------


def _linear(channel: int) -> float:
    value = channel / 255
    return value / 12.92 if value <= 0.03928 else ((value + 0.055) / 1.055) ** 2.4


def luminance(colour: str) -> float:
    """WCAG relative luminance of ``#rgb`` or ``#rrggbb``."""
    digits = colour.strip().lstrip("#")
    if len(digits) in (3, 4):
        digits = "".join(character * 2 for character in digits)
    red, green, blue = (int(digits[index:index + 2], 16) for index in (0, 2, 4))
    return 0.2126 * _linear(red) + 0.7152 * _linear(green) + 0.0722 * _linear(blue)


def contrast_ratio(first: str, second: str) -> float:
    """WCAG contrast ratio of two colours, from 1 to 21."""
    lighter, darker = sorted((luminance(first), luminance(second)), reverse=True)
    return (lighter + 0.05) / (darker + 0.05)


def check_contrast(tokens: dict[str, Any], pairs: list[Any]) -> list[Problem]:
    """Check token pairs in the light and dark sets."""
    problems: list[Problem] = []

    def add(message: str) -> None:
        problems.append(("tokens", "contrast", message))

    colours = tokens.get("color", {}) if isinstance(tokens, dict) else {}
    for mode in ("light", "dark"):
        palette = colours.get(mode)
        if not isinstance(palette, dict):
            add(f"the tokens have no color.{mode} set")
            continue
        for pair in pairs:
            if not (isinstance(pair, list) and len(pair) == 3):
                add(f"bad contrast_pairs entry {pair!r}")
                continue
            foreground, background, minimum = pair
            missing = [token for token in (foreground, background) if token not in palette]
            for token in missing:
                add(f'{mode}: unknown token "{token}"')
            if missing:
                continue
            try:
                ratio = contrast_ratio(palette[foreground], palette[background])
            except (ValueError, IndexError):
                add(f"{mode}: {foreground} or {background} is not a hex colour")
                continue
            if ratio < minimum:
                add(f"{mode}: {foreground} on {background} is {ratio:.2f}:1, need {minimum}:1")
    return list(dict.fromkeys(problems))


# ---------------------------------------------------------------------------
# Whole site
# ---------------------------------------------------------------------------


@dataclass
class Report:
    problems: list[Problem]
    pages: int
    internal: int
    external: int


def check_titles(pages: dict[str, Page]) -> list[Problem]:
    """Page titles must be unique."""
    by_title: dict[str, list[str]] = {}
    for name, page in pages.items():
        title = page.title()
        if title:
            by_title.setdefault(title.casefold(), []).append(name)
    problems: list[Problem] = []
    for names in by_title.values():
        for name in names:
            others = ", ".join(sorted(other for other in names if other != name))
            if others:
                title = pages[name].title()
                problems.append((name, "title", f'the title "{title}" is also used by {others}'))
    return problems


def check_drafts(pages: dict[str, Page], draft_pages: list[str]) -> list[Problem]:
    """Draft pages must say so in their visible text."""
    problems: list[Problem] = []
    for name in draft_pages:
        page = pages.get(name)
        if page is None:
            problems.append((name, "draft-missing", "the draft page does not exist"))
        elif not re.search(r"\bDRAFT\b", " ".join(page.body_text)):
            problems.append((name, "draft-missing", "the page does not contain the word DRAFT"))
    return problems


def read_text(path: Path) -> tuple[str, str | None]:
    """Read UTF-8 text; the second item describes a decoding problem."""
    data = path.read_bytes()
    try:
        return data.decode("utf-8"), None
    except UnicodeDecodeError as error:
        return data.decode("utf-8", errors="replace"), f"the file is not valid UTF-8: {error}"


def load_tokens(path: Path) -> tuple[dict[str, Any] | None, str]:
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return None, str(error)
    return (loaded, "") if isinstance(loaded, dict) else (None, "not a JSON object")


def check_site(
    root: Path,
    config: dict[str, Any],
    tokens_path: Path | None = None,
    external: bool = False,
    fetch: Fetch | None = None,
) -> Report:
    """Run every check on the built site in ``root``."""
    files = {
        path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()
    }
    problems: list[Problem] = []
    pages: dict[str, Page] = {}
    for name in sorted(files):
        if name.endswith(".html"):
            text, decode_problem = read_text(root / name)
            if decode_problem:
                problems.append((name, "parse", decode_problem))
            pages[name] = parse_page(text)
    css_files = {name: read_text(root / name)[0] for name in sorted(files) if name.endswith(".css")}
    hosts = config.get("allowed_link_hosts")
    site = Site(
        files=files,
        pages=pages,
        external_paths=tuple(str(item) for item in config.get("external_paths", [])),
        allowed_hosts=None if hosts is None else frozenset(str(h).lower() for h in hosts),
    )
    targets: dict[str, set[str]] = {}
    internal = 0
    urls: dict[str, set[str]] = {}
    for name, page in sorted(pages.items()):
        problems.extend((name, "parse", message) for message in page.parse_problems)
        problems.extend(check_accessibility(name, page))
        problems.extend(check_policy(name, page, files))
        result = check_refs(name, page, site)
        problems.extend(result.problems)
        internal += result.internal
        targets[name] = result.targets
        urls[name] = result.external
    problems.extend(check_titles(pages))
    problems.extend(check_orphans(pages, targets))
    problems.extend(check_css(css_files, files))
    problems.extend(check_drafts(pages, [str(n) for n in config.get("draft_pages", [])]))
    pairs = config.get("contrast_pairs", [])
    if pairs:
        tokens, error = load_tokens(tokens_path) if tokens_path else (None, "no tokens file")
        if tokens is None:
            problems.append(("tokens", "contrast", f"cannot read the tokens file: {error}"))
        else:
            problems.extend(check_contrast(tokens, pairs))
    if external:
        problems.extend(check_external(urls, fetch or fetch_url))
    distinct = {url.split("#", 1)[0] for found in urls.values() for url in found}
    return Report(sorted(set(problems)), len(pages), internal, len(distinct))


def format_problem(problem: Problem) -> str:
    return f"{problem[0]}: {problem[1]}: {problem[2]}"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Check a built static website.")
    parser.add_argument("dir", nargs="?", type=Path, default=None,
                        help="built site (default: build/website)")
    parser.add_argument("--config", type=Path, default=None,
                        help="site configuration (default: website/site.json)")
    parser.add_argument("--tokens", type=Path, default=None,
                        help="design tokens JSON for the contrast checks")
    parser.add_argument("--external", action="store_true",
                        help="also fetch every https link (needs the network)")
    return parser


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    args = build_parser().parse_args(argv)
    root = args.dir if args.dir is not None else DEFAULT_SITE_DIR
    if not root.is_dir():
        print(f"site_check: {root} is not a directory", file=sys.stderr)
        return 2
    if not any(root.rglob("*.html")):
        print(f"site_check: {root} holds no HTML page", file=sys.stderr)
        return 2
    config_path = args.config if args.config is not None else DEFAULT_CONFIG
    config: dict[str, Any] = {}
    if config_path.is_file():
        try:
            config = json.loads(config_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            print(f"site_check: cannot read {config_path}: {error}", file=sys.stderr)
            return 2
    elif args.config is not None:
        print(f"site_check: {config_path} does not exist", file=sys.stderr)
        return 2
    if not isinstance(config, dict):
        print(f"site_check: {config_path} is not a JSON object", file=sys.stderr)
        return 2
    tokens = args.tokens or (REPO_ROOT / str(config.get("tokens", DEFAULT_TOKENS)))
    report = check_site(root, config, tokens, args.external)
    for problem in report.problems:
        print(format_problem(problem))
    if report.problems:
        print(f"site_check: {len(report.problems)} problems in {report.pages} pages")
        return 1
    print(
        f"site_check: {report.pages} pages, {report.internal} internal links, "
        f"{report.external} external links, all checks passed"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
