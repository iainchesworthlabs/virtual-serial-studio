#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the built documentation site: the front pages are plain HTML, load nothing from other sites, and link to files that exist.

    python3 tools/ci/check_site_pages.py [--site site] [--root .]

Without --site it builds the site with `mkdocs build --strict` into a temporary folder (it needs the packages of
requirements/docs.txt) and checks that. With --site it checks a folder that is already built.

Every page:
- has a <title>, exactly one <h1> (the first heading of the main content), headings that do not skip a level, and a canonical address;
- has an alternative text on every picture;
- links to a file that exists, with an anchor that exists in it. Links that start with "/" are the company shell's
  (/, /support/, /assets/v1/...) and are not followed, but none of the page's own may start with
  /virtual-serial-studio/ (the pages must work under a preview address, so the pages of this site use relative links). The links inside the company header and footer (the kit's, which
  link to the apps' pages as every page of the company site does) are not the page's and are not checked. An absolute link is allowed
  to GitHub and to gnu.org only on a front page.

The front pages (tools/docs/theme/marketing.html), which run under a preview that blocks every script, also have

- lang="en-AU", a description, and a title and description of their own (no other page has the same);
- no <script> except a JSON-LD data block, no <style> and no style= attribute;
- a width and a height on every picture;
- nothing loaded from another host: no src, srcset, stylesheet link or CSS url() or @import that starts with http(s):// or //;
- no mailto: link (an address is written as plain text; enquiries go through the company's contact form);
- at most one <form>, which is the contact form: method post to the company site's endpoint /api/contact and nowhere else, with exactly
  the fields the endpoint accepts (FORM_FIELDS), a label for each, the limits and the topics of the endpoint, a honeypot field that no
  person sees or tabs to, and a consent box that links to the company's privacy text. The guide's own pages (Material's search) are not
  front pages and are not looked at.

And the pages and files that the company site's contract asks for, and that other pages lead to, exist: see FIXED. The company's contract
asks for /virtual-serial-studio/privacy/ and /virtual-serial-studio/support/, and they are in FIXED.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
import tempfile
from pathlib import Path
from urllib.parse import unquote, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import site_tools  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
FIXED = ["index.html", "guide/index.html", "developers/index.html", "downloads/index.html", "privacy/index.html", "support/index.html",
         "theme.css", "icon.svg", "icl-app.json"]
# The pages that are drafts until the owner says they are not: the licence and privacy texts are for a solicitor, and the prices are not
# decided (docs/plan/site.md, decision D). While one of them is built it must say DRAFT in its main content. One that is not built yet
# is not an error.
DRAFT_PAGES = ["licensing/", "privacy/", "pricing/"]
DRAFT_WORD = re.compile(r"\bDRAFT\b")
ALLOWED_HOSTS = {"github.com", "www.gnu.org"}

# The contact form. The endpoint is the company site's (iainchesworthlabs/icl-site: infra/lambda/contact/config.ts and
# docs/design/contact-form.md). Its field names, limits and topics are copied here, so that a change to either side is noticed.
FORM_ACTION = "/api/contact"
FORM_SOURCE = "virtual-serial-studio"
FORM_FIELDS = {"source", "name", "organisation", "email", "topic", "message", "ref_code", "consent"}
FORM_REQUIRED = {"name", "email", "topic", "message", "consent"}
FORM_MAXLENGTH = {"name": "100", "organisation": "150", "email": "254", "message": "4000"}
FORM_TOPICS = {"", "pro", "enterprise", "trial", "plugins", "other"}
FORM_HONEYPOT = "ref_code"
FORM_PRIVACY = "/privacy/#contact-form"


def check_headings(where: str, levels: list[int]) -> list[str]:
    problems: list[str] = []
    if levels and levels[0] != 1:
        problems.append(f"{where}: the first heading is an h{levels[0]}, not the h1")
    for before, after in zip(levels, levels[1:]):
        if after > before + 1:
            problems.append(f"{where}: a heading goes from h{before} to h{after}; headings do not skip a level")
            break
    return problems


def check_forms(where: str, page: "site_tools.Page") -> list[str]:
    """The contact form of a front page, held to the contract of the company's endpoint."""
    problems: list[str] = []
    if len(page.forms) > 1:
        problems.append(f"{where}: has {len(page.forms)} forms; the only form is the contact form")
    for form in page.forms[:1]:
        attrs, controls = form["attrs"], form["controls"]
        if attrs.get("method", "get").lower() != "post":
            problems.append(f'{where}: the form must have method="post"')
        if attrs.get("action", "") != FORM_ACTION:
            problems.append(f'{where}: the form posts to {attrs.get("action", "")!r}; it may post only to {FORM_ACTION} (the endpoint of the company site)')
        if attrs.get("enctype", "application/x-www-form-urlencoded").lower() != "application/x-www-form-urlencoded":
            problems.append(f"{where}: the form's enctype must be the default; the endpoint reads application/x-www-form-urlencoded only")
        named = [c for c in controls if c["attrs"].get("name")]
        names = [c["attrs"]["name"] for c in named]
        if len(set(names)) != len(names):
            problems.append(f"{where}: the form names a field twice: {sorted({n for n in names if names.count(n) > 1})}")
        if set(names) != FORM_FIELDS:
            problems.append(f"{where}: the form's fields are {sorted(set(names))}; the endpoint takes {sorted(FORM_FIELDS)}")
        by_name = {c["attrs"]["name"]: c for c in named}
        source = by_name.get("source")
        if source and (source["attrs"].get("type") != "hidden" or source["attrs"].get("value") != FORM_SOURCE):
            problems.append(f'{where}: the field "source" must be a hidden field with the value "{FORM_SOURCE}"')
        for control in controls:
            kind = control["attrs"].get("type", "text")
            if control["tag"] == "button" or kind in ("hidden", "submit"):
                continue
            if control["attrs"].get("id", "\0") not in page.label_for:
                problems.append(f'{where}: the field "{control["attrs"].get("name", "?")}" has no <label for> that names its id')
        for name in sorted(FORM_REQUIRED & by_name.keys()):
            if "required" not in by_name[name]["attrs"]:
                problems.append(f'{where}: the field "{name}" is required by the endpoint and needs the required attribute')
        for name, limit in FORM_MAXLENGTH.items():
            if name in by_name and by_name[name]["attrs"].get("maxlength") != limit:
                problems.append(f'{where}: the field "{name}" needs maxlength="{limit}", the limit of the endpoint')
        if "email" in by_name and by_name["email"]["attrs"].get("type") != "email":
            problems.append(f'{where}: the field "email" needs type="email"')
        if "message" in by_name and by_name["message"]["tag"] != "textarea":
            problems.append(f'{where}: the field "message" must be a <textarea>')
        if "topic" in by_name:
            topic = by_name["topic"]
            if topic["tag"] != "select" or set(topic["options"]) != FORM_TOPICS:
                problems.append(f'{where}: the field "topic" must be a <select> whose options are {sorted(FORM_TOPICS - {""})} and an empty first one')
        consent = by_name.get("consent")
        if consent and (consent["attrs"].get("type") != "checkbox" or consent["attrs"].get("value") != "yes"):
            problems.append(f'{where}: the field "consent" must be a checkbox with the value "yes"')
        trap = by_name.get(FORM_HONEYPOT)
        if trap:
            a = trap["attrs"]
            if not trap["hidden"] or a.get("type", "text") != "text" or a.get("tabindex") != "-1" or a.get("autocomplete") != "off" or "required" in a:
                problems.append(f'{where}: the honeypot "{FORM_HONEYPOT}" must be a text field inside an aria-hidden element, with tabindex="-1" and '
                                'autocomplete="off", and not required')
        if not any(c["tag"] == "button" and c["attrs"].get("type", "submit") == "submit" for c in controls):
            problems.append(f"{where}: the form has no submit button")
        if FORM_PRIVACY not in form["links"]:
            problems.append(f"{where}: the form must link to {FORM_PRIVACY} (the company's text about what the form keeps)")
    return problems


def check_site_drafts(site: Path) -> list[str]:
    """Each of DRAFT_PAGES that is in the built site still has the word DRAFT in its main content."""
    problems: list[str] = []
    for address in DRAFT_PAGES:
        path = site / address / "index.html"
        if path.is_file() and not DRAFT_WORD.search(" ".join(site_tools.parse(path).main_text)):
            problems.append(f"{address}index.html: the main content does not contain the word DRAFT. The licence, privacy and pricing pages keep "
                            "their DRAFT marks until the owner says they can go")
    return problems


def absent_drafts(site: Path) -> list[str]:
    """The addresses of DRAFT_PAGES that are not in the built site (yet)."""
    return [address for address in DRAFT_PAGES if not (site / address / "index.html").is_file()]


def check_site(site: Path, root: Path) -> list[str]:
    problems: list[str] = check_site_drafts(site)
    for name in FIXED:
        if not (site / name).is_file():
            problems.append(f"{name}: is not in the built site (the company's contract requires it, or other pages lead to it)")
    pages = {p: site_tools.parse(p) for p in sorted(site.rglob("*.html"))}
    titles: dict[str, str] = {}
    descriptions: dict[str, str] = {}
    for path, page in pages.items():
        rel = path.relative_to(site).as_posix()
        if rel == "404.html" or rel == "search/index.html":
            continue
        front = not page.material
        where = rel
        if not page.title.strip():
            problems.append(f"{where}: no <title>")
        if page.h1 != 1:
            problems.append(f"{where}: has {page.h1} <h1>, not one")
        if not page.canonical:
            problems.append(f"{where}: no canonical address")
        problems += check_headings(where, page.headings)
        for image in page.images:
            if "alt" not in image:
                problems.append(f"{where}: a picture ({image.get('src', '?')}) has no alt attribute")
        if front:
            if page.lang != "en-AU":
                problems.append(f'{where}: <html> needs lang="en-AU"')
            if not page.description.strip():
                problems.append(f"{where}: no description")
            elif page.description in descriptions:
                problems.append(f"{where}: has the same description as {descriptions[page.description]}")
            else:
                descriptions[page.description] = where
            if page.title in titles:
                problems.append(f"{where}: has the same title as {titles[page.title]}")
            else:
                titles[page.title] = where
            for kind in page.scripts:
                if kind != "application/ld+json":
                    problems.append(f"{where}: has a <script>; the front pages are script-free (a preview blocks scripts)")
            if page.styles or page.style_attrs:
                problems.append(f"{where}: has <style> or a style= attribute; put it in docs/assets/vss.css")
            for image in page.images:
                if not image.get("width") or not image.get("height"):
                    problems.append(f"{where}: a picture ({image.get('src', '?')}) has no width and height")
            problems += check_forms(where, page)
        for tag, attribute, value in page.refs:
            value = value.strip()
            if not value or value.startswith(("data:", "javascript:")):
                continue
            if value.startswith("mailto:"):
                problems.append(f"{where}: {value}: no mailto: link; write an address as plain text, or use the company's contact form")
                continue
            parts = urlparse(value)
            if parts.scheme in ("http", "https") or value.startswith("//"):
                if not front:
                    continue  # the guide's own links (Material's credit in the footer, the repository)
                if parts.scheme not in ("http", "https") or parts.netloc not in ALLOWED_HOSTS:
                    problems.append(f"{where}: {value}: not a link to a host this site may name ({', '.join(sorted(ALLOWED_HOSTS))})")
                elif attribute == "srcset" or (tag != "a" and tag != "link"):
                    problems.append(f"{where}: {value}: loads something from another host")
                elif tag == "link" and "icon" not in value:
                    problems.append(f"{where}: {value}: a <link> to another host")
                continue
            if value.startswith("/"):
                if value.startswith("/virtual-serial-studio/"):
                    problems.append(f"{where}: {value}: write a relative link, so the page works under a preview address")
                continue
            if parts.scheme:
                problems.append(f"{where}: {value}: unknown link type")
                continue
            target = site_tools.target_file(site, path, value)
            if target is None or not target.exists():
                problems.append(f"{where}: {value}: no such file in the built site")
                continue
            if parts.fragment and target.suffix == ".html":
                other = pages.get(target) or site_tools.parse(target)
                if unquote(parts.fragment) not in other.ids:
                    problems.append(f"{where}: {value}: no anchor #{parts.fragment} in {target.relative_to(site).as_posix()}")
    for name in ("theme.css", "assets/vss.css", "assets/guide.css", "assets/icl.css"):
        css = site / name
        if css.is_file():
            text = re.sub(r"/\*.*?\*/", "", css.read_text(encoding="utf-8"), flags=re.S)
            if re.search(r"@import|url\(\s*['\"]?(https?:)?//", text):
                problems.append(f"{name}: loads something with @import or an address; the site loads nothing from other sites")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--site", type=Path, help="a site that is already built")
    args = parser.parse_args(argv)
    tmp = None
    site = args.site
    if site is None:
        if not site_tools.material_installed():
            print("check_site_pages: mkdocs-material is not installed (pip install --require-hashes -r requirements/docs.txt): cannot build the site", file=sys.stderr)
            return 2
        tmp = tempfile.mkdtemp(prefix="vss-site-")
        site = Path(tmp)
        if site_tools.build(args.root, site) != 0:
            shutil.rmtree(tmp, ignore_errors=True)
            return 1
    try:
        problems = check_site(site, args.root)
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
    for problem in problems:
        print(f"::error::{problem}")
    if problems:
        return 1
    absent = absent_drafts(site) if tmp is None else []
    print("Site pages: ok")
    if absent:
        print(f"Site drafts: {', '.join(absent)} not in the built site yet, so the DRAFT check has nothing to look at there")
    return 0


if __name__ == "__main__":
    sys.exit(main())
