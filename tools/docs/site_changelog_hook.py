# SPDX-License-Identifier: GPL-3.0-or-later
"""MkDocs hook: the released sections of CHANGELOG.md, for the "What's new" page.

A page writes this marker on a line of its own, and nothing else is changed:

    <!-- changelog:releases -->

The hook replaces it with the released sections of CHANGELOG.md as Markdown. CHANGELOG.md is the one source, written by
`tools/changelog.py` when a release is made, so the page is never typed by hand and says nothing that the changelog does not.

- Each released section (`## [1.2.3] - 2026-12-01`) becomes a heading, `## 1.2.3, 1 December 2026`, with an address of its own
  (`#v1-2-3`), a link to the release on GitHub, and the section's text. The page that holds the marker has the one `h1`, so the
  headings of the changelog are shifted to start at `first_level` (2) and the categories under a version (`### Added`) stay one
  level below it. A heading inside an entry is never above that, whatever it was in the changelog.
- The `## [Unreleased]` section is not a release and is left out, and so are the link definitions at the foot of the file. Fragments
  that wait in `changelog.d/` are not read: they are not released.
- A relative link in an entry is relative to the repository, where CHANGELOG.md is. It becomes a link to the file on GitHub, so it
  resolves from the page. A link that starts with `http` is left as it is, and the page check allows GitHub and gnu.org only.
- With no released section the marker becomes one sentence that says so. When the changelog then has entries under Unreleased they
  are shown under an Unreleased heading, because that is what the file has.
- An unknown marker (`changelog:` and any other name) stops the build, so a typo is not left in a page.

The module has no MkDocs import, so the tests use it as it is.
"""

from __future__ import annotations

import re
from pathlib import Path

CHANGELOG = "CHANGELOG.md"
# develop and not main, as tools/docs/mkdocs_hooks.py: the pages are built from the trunk.
BRANCH = "develop"
FIRST_LEVEL = 2

MARKER_RE = re.compile(r"<!--\s*changelog:([a-z]*)\s*-->")
UNRELEASED_RE = re.compile(r"^## \[Unreleased\]\s*$")
VERSION_RE = re.compile(r"^## \[(?P<version>[0-9]+\.[0-9]+\.[0-9]+)\](?:\s*-\s*(?P<date>[0-9]{4}-[0-9]{2}-[0-9]{2}))?\s*$")
ANY_H2_RE = re.compile(r"^## \[")
LINK_DEFINITION_RE = re.compile(r"^\[[^\]]+\]:\s*\S+\s*$")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
# [text](target) and ![alt](target); the target has no spaces or parentheses in this changelog.
LINK_RE = re.compile(r"(\]\()([^)\s#]*)(#[^)\s]*)?(\))")
SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")

MONTHS = (
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
)

NO_RELEASE = "The changelog has no released version yet, so there is nothing to list here."


class ChangelogError(ValueError):
    """A problem with a page's marker that the person who edits the page has to fix."""


def long_date(iso: str) -> str:
    """2026-12-01 as 1 December 2026."""
    year, month, day = (int(part) for part in iso.split("-"))
    return f"{day} {MONTHS[month - 1]} {year}"


def sections(text: str) -> tuple[list[str] | None, list[tuple[str, str | None, list[str]]]]:
    """The Unreleased body (None when the file has no such section) and the released sections, each (version, date, body), in file order."""
    lines = text.splitlines()
    while lines and (not lines[-1].strip() or LINK_DEFINITION_RE.match(lines[-1])):
        lines.pop()
    unreleased: list[str] | None = None
    released: list[tuple[str, str | None, list[str]]] = []
    current: list[str] | None = None
    fenced = False
    for line in lines:
        if FENCE_RE.match(line):
            fenced = not fenced
        elif not fenced and ANY_H2_RE.match(line):
            if UNRELEASED_RE.match(line):
                unreleased = current = []
            else:
                found = VERSION_RE.match(line)
                if not found:
                    raise ChangelogError(f"{CHANGELOG}: the heading {line!r} is neither [Unreleased] nor [X.Y.Z] - date")
                current = []
                released.append((found.group("version"), found.group("date"), current))
            continue
        if current is not None:
            current.append(line)
    return unreleased, released


def shift_and_link(body: list[str], first_level: int, repo_url: str) -> list[str]:
    """The body of a section with its headings shifted below `first_level` and its relative links made links to GitHub."""
    out: list[str] = []
    fenced = False
    for line in body:
        if FENCE_RE.match(line):
            fenced = not fenced
            out.append(line)
            continue
        if fenced:
            out.append(line)
            continue
        heading = HEADING_RE.match(line)
        if heading:
            # The changelog's categories are `###`, one below its versions (`##`). They keep that distance.
            level = min(max(len(heading.group(1)) + first_level - 2, first_level + 1), 6)
            line = f"{'#' * level} {heading.group(2)}"
        out.append(LINK_RE.sub(lambda found: github_link(found, repo_url), line))
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    return out


def github_link(found: re.Match[str], repo_url: str) -> str:
    opening, target, anchor, closing = found.groups()
    if not target or target.startswith("/") or SCHEME_RE.match(target):
        return found.group(0)
    path = target[2:] if target.startswith("./") else target
    kind = "tree" if path.endswith("/") else "blob"
    return f"{opening}{repo_url}/{kind}/{BRANCH}/{path.rstrip('/')}{anchor or ''}{closing}"


def render(text: str, repo_url: str, first_level: int = FIRST_LEVEL) -> str:
    """The Markdown that replaces the marker."""
    repo_url = repo_url.rstrip("/")
    unreleased, released = sections(text)
    parts: list[str] = []
    for version, date, body in released:
        content = shift_and_link(body, first_level, repo_url)
        if not content:
            continue
        title = f"{version}, {long_date(date)}" if date else version
        parts.append(
            f"{'#' * first_level} {title} {{ #v{version.replace('.', '-')} }}\n\n"
            f"[This release on GitHub]({repo_url}/releases/tag/v{version})\n\n" + "\n".join(content)
        )
    if not parts:
        parts.append(NO_RELEASE)
        waiting = shift_and_link(unreleased or [], first_level, repo_url)
        if waiting:
            parts.append(f"{'#' * first_level} Unreleased {{ #unreleased }}\n\nNot released yet.\n\n" + "\n".join(waiting))
    return "\n\n".join(parts)


def fill(markdown: str, changelog_text: str, repo_url: str) -> str:
    """The page's Markdown with the marker replaced. A page without the marker comes back unchanged."""

    def replace(found: re.Match[str]) -> str:
        if found.group(1) != "releases":
            raise ChangelogError(f"unknown marker <!-- changelog:{found.group(1)} -->; the only one is <!-- changelog:releases -->")
        return render(changelog_text, repo_url)

    return MARKER_RE.sub(replace, markdown)


def on_page_markdown(markdown, page, config, files):  # noqa: ANN001, ANN201 (MkDocs passes its own types)
    if not MARKER_RE.search(markdown):
        return markdown
    root = Path(config["docs_dir"]).resolve().parent
    text = (root / CHANGELOG).read_text(encoding="utf-8")
    return fill(markdown, text, config["repo_url"])
