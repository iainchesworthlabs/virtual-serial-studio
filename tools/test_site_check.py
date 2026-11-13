#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/site_check.py.

Run with:

    python -m unittest discover -s tools -p "test_site_check.py" -v

Each rule has a failing and a passing snippet.  Pages are built from a small
valid template, so a test that expects one rule also shows that nothing else
fires.  Site trees are written to a temporary directory and no test uses the
network: ``--external`` is exercised with a fake fetch function.
"""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import site_check  # noqa: E402

CSS = """\
:root{--ink:#14171c;--ground:#ffffff}
@media (prefers-color-scheme:dark){:root{--ink:#eeeeee;--ground:#000000}}
body{color:var(--ink);background:var(--ground);font-size:1rem}
a:focus-visible{outline:2px solid currentColor}
@media (prefers-reduced-motion:no-preference){a{transition:color .2s}}
"""

TOKENS = {
    "color": {
        "light": {"ink": "#000000", "ground": "#ffffff", "pale": "#cccccc"},
        "dark": {"ink": "#ffffff", "ground": "#000000", "pale": "#111111"},
    }
}


def page_html(
    body: str = "<p>Hello.</p>",
    head: str = "",
    title: str = "Home",
    nav: str = '<a href="index.html">Home</a>',
    h1: str = "Welcome",
    html_attrs: str = ' lang="en"',
) -> str:
    """A page that passes every page-level rule."""
    return f"""<!DOCTYPE html>
<html{html_attrs}>
<head>
<meta charset="utf-8">
<title>{title}</title>
<meta name="description" content="A test page.">
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" href="css/site.css">
{head}
</head>
<body>
<a class="skip" href="#main">Skip to content</a>
<header><nav aria-label="Primary">{nav}</nav></header>
<main id="main">
<h1>{h1}</h1>
{body}
</main>
<footer><p>Footer</p></footer>
</body>
</html>
"""


def page_rules(html: str) -> set[str]:
    """Rule ids reported for one page, without the site-wide checks."""
    page = site_check.parse_page(html)
    found = site_check.check_accessibility("p.html", page)
    found += site_check.check_policy("p.html", page, set())
    # Existence is covered by the tree tests, so only judge the kind of URL here.
    site = site_check.Site(files={"css/site.css", "p.html"}, pages={})
    refs = site_check.check_refs("p.html", page, site).problems
    found += [item for item in refs if item[1] not in ("link-broken", "link-fragment")]
    found += [("p.html", "parse", message) for message in page.parse_problems]
    return {rule for _, rule, _ in found}


def css_rules(files: dict[str, str], known: set[str] | None = None) -> set[str]:
    found = site_check.check_css(files, known or set())
    return {rule for _, rule, _ in found}


class SiteCase(unittest.TestCase):
    """Base class that owns a temporary site directory."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name) / "site"
        self.root.mkdir()

    def write(self, files: dict[str, str]) -> None:
        for name, text in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")

    def check(
        self,
        files: dict[str, str],
        config: dict | None = None,
        **kwargs: object,
    ) -> list[site_check.Problem]:
        self.write(files)
        report = site_check.check_site(self.root, config or {}, **kwargs)  # type: ignore[arg-type]
        return report.problems

    def rules(self, files: dict[str, str], config: dict | None = None) -> set[str]:
        return {rule for _, rule, _ in self.check(files, config)}


def two_pages() -> dict[str, str]:
    return {
        "index.html": page_html(
            body='<p><a href="about.html">About us</a></p>',
            nav='<a href="index.html">Home</a>',
        ),
        "about.html": page_html(title="About", h1="About", body="<p>About text.</p>"),
        "css/site.css": CSS,
    }


class ContrastMathsTests(unittest.TestCase):
    def test_black_on_white_is_21(self) -> None:
        self.assertAlmostEqual(site_check.contrast_ratio("#000000", "#ffffff"), 21.0)

    def test_grey_767676_on_white(self) -> None:
        self.assertAlmostEqual(site_check.contrast_ratio("#767676", "#FFFFFF"), 4.54, 2)

    def test_same_colour_is_one_and_order_does_not_matter(self) -> None:
        self.assertAlmostEqual(site_check.contrast_ratio("#123456", "#123456"), 1.0)
        self.assertAlmostEqual(
            site_check.contrast_ratio("#fff", "#000"),
            site_check.contrast_ratio("#000", "#fff"),
        )

    def test_pairs_pass_and_fail_in_both_sets(self) -> None:
        passing = site_check.check_contrast(TOKENS, [["ink", "ground", 4.5]])
        self.assertEqual(passing, [])
        failing = site_check.check_contrast(TOKENS, [["pale", "ground", 3.0]])
        self.assertEqual({rule for _, rule, _ in failing}, {"contrast"})
        self.assertEqual({file for file, _, _ in failing}, {"tokens"})
        text = " ".join(message for _, _, message in failing)
        self.assertIn("light:", text)
        self.assertIn("dark:", text)

    def test_unknown_token_is_a_problem(self) -> None:
        found = site_check.check_contrast(TOKENS, [["ink", "nope", 4.5]])
        self.assertTrue(any('unknown token "nope"' in message for _, _, message in found))


class HeadTests(unittest.TestCase):
    def test_template_is_clean(self) -> None:
        self.assertEqual(page_rules(page_html()), set())

    def test_lang(self) -> None:
        self.assertIn("lang", page_rules(page_html(html_attrs="")))
        self.assertIn("lang", page_rules(page_html(html_attrs=' lang=""')))
        self.assertNotIn("lang", page_rules(page_html()))

    def test_title(self) -> None:
        self.assertIn("title", page_rules(page_html(title="")))
        self.assertIn("title", page_rules(page_html().replace("<title>Home</title>", "")))
        self.assertNotIn("title", page_rules(page_html(title="Fine")))

    def test_title_unique_across_pages(self) -> None:
        pages = {
            "a.html": site_check.parse_page(page_html(title="Same")),
            "b.html": site_check.parse_page(page_html(title="same")),
            "c.html": site_check.parse_page(page_html(title="Other")),
        }
        found = site_check.check_titles(pages)
        self.assertEqual({file for file, _, _ in found}, {"a.html", "b.html"})
        self.assertEqual({rule for _, rule, _ in found}, {"title"})

    def test_description(self) -> None:
        good = page_html()
        empty = good.replace('content="A test page."', 'content=""')
        self.assertIn("description", page_rules(empty))
        self.assertIn(
            "description",
            page_rules(good.replace('<meta name="description" content="A test page.">', "")),
        )
        self.assertNotIn("description", page_rules(good))

    def test_viewport(self) -> None:
        good = page_html()
        self.assertIn("viewport", page_rules(good.replace('name="viewport"', 'name="other"')))
        zoom = good.replace("initial-scale=1", "user-scalable=no")
        self.assertIn("viewport", page_rules(zoom))
        capped = good.replace("initial-scale=1", "maximum-scale=1")
        self.assertIn("viewport", page_rules(capped))
        allowed = good.replace("initial-scale=1", "maximum-scale=5")
        self.assertNotIn("viewport", page_rules(allowed))


class StructureTests(unittest.TestCase):
    def test_h1(self) -> None:
        self.assertIn("h1", page_rules(page_html(body="<h1>Again</h1>")))
        self.assertIn("h1", page_rules(page_html().replace("<h1>Welcome</h1>", "")))
        self.assertNotIn("h1", page_rules(page_html()))

    def test_heading_order(self) -> None:
        self.assertIn("heading-order", page_rules(page_html(body="<h2>A</h2><h4>B</h4>")))
        fine = "<h2>A</h2><h3>B</h3><h2>C</h2><h3>D</h3>"
        self.assertNotIn("heading-order", page_rules(page_html(body=fine)))

    def test_heading_empty(self) -> None:
        self.assertIn("heading-empty", page_rules(page_html(body="<h2> </h2>")))
        self.assertNotIn("heading-empty", page_rules(page_html(body="<h2>Words</h2>")))

    def test_landmark_main(self) -> None:
        good = page_html()
        no_main = good.replace('<main id="main">', "<div>").replace("</main>", "</div>")
        self.assertIn("landmark", page_rules(no_main))
        self.assertIn("landmark", page_rules(good.replace('<main id="main">', "<main>")))
        self.assertIn("landmark", page_rules(page_html(body="<main id='m2'><p>x</p></main>")))

    def test_landmark_header_nav_footer(self) -> None:
        good = page_html()
        for tag in ("header", "footer", "nav"):
            with self.subTest(tag=tag):
                plain = good.replace(f"<{tag}", "<div").replace(f"</{tag}>", "</div>")
                self.assertIn("landmark", page_rules(plain))

    def test_several_navs_need_distinct_labels(self) -> None:
        extra = '<nav aria-label="Footer"><a href="index.html">Home</a></nav>'
        self.assertNotIn("landmark", page_rules(page_html(body=extra)))
        unlabelled = "<nav><a href='index.html'>Home</a></nav>"
        self.assertIn("landmark", page_rules(page_html(body=unlabelled)))
        same = '<nav aria-label="primary"><a href="index.html">Home</a></nav>'
        self.assertIn("landmark", page_rules(page_html(body=same)))

    def test_skip_link(self) -> None:
        good = page_html()
        self.assertNotIn("skip-link", page_rules(good))
        self.assertIn("skip-link", page_rules(good.replace('href="#main"', 'href="#other"')))
        first = good.replace('<a class="skip" href="#main">Skip to content</a>', "")
        self.assertIn("skip-link", page_rules(first))
        nothing = first.replace("<a href=\"index.html\">Home</a>", "Home")
        self.assertIn("skip-link", page_rules(nothing))


class ImageTests(unittest.TestCase):
    def test_img_alt(self) -> None:
        self.assertIn("img-alt", page_rules(page_html(body='<img src="img/a.png">')))
        self.assertNotIn("img-alt", page_rules(page_html(body='<img src="img/a.png" alt="">')))
        self.assertNotIn("img-alt", page_rules(page_html(body='<img src="img/a.png" alt="A">')))

    def test_svg(self) -> None:
        self.assertIn("img-alt", page_rules(page_html(body="<svg><path d='M0 0'/></svg>")))
        hidden = '<svg aria-hidden="true"><path d="M0 0"/></svg>'
        self.assertNotIn("img-alt", page_rules(page_html(body=hidden)))
        named = '<svg role="img" aria-label="Logo"><path d="M0 0"/></svg>'
        self.assertNotIn("img-alt", page_rules(page_html(body=named)))
        titled = "<svg role='img'><title>Logo</title><path d='M0 0'/></svg>"
        self.assertNotIn("img-alt", page_rules(page_html(body=titled)))

    def test_role_img_needs_a_name(self) -> None:
        self.assertIn("img-alt", page_rules(page_html(body='<span role="img">x</span>')))
        named = '<span role="img" aria-label="Star">x</span>'
        self.assertNotIn("img-alt", page_rules(page_html(body=named)))


class NameTests(unittest.TestCase):
    def test_link_name(self) -> None:
        self.assertIn("link-name", page_rules(page_html(body='<a href="x.html"></a>')))
        hidden_text = '<a href="x.html"><span aria-hidden="true">Go</span></a>'
        self.assertIn("link-name", page_rules(page_html(body=hidden_text)))
        self.assertNotIn("link-name", page_rules(page_html(body='<a href="x.html">Go</a>')))
        label = '<a href="x.html" aria-label="Go"><span aria-hidden="true">x</span></a>'
        self.assertNotIn("link-name", page_rules(page_html(body=label)))
        image = '<a href="x.html"><img src="a.png" alt="Logo"></a>'
        self.assertNotIn("link-name", page_rules(page_html(body=image)))
        blank_image = '<a href="x.html"><img src="a.png" alt=""></a>'
        self.assertIn("link-name", page_rules(page_html(body=blank_image)))

    def test_link_vague(self) -> None:
        for words in ("Click here", "HERE", "Read more", "more", "Link"):
            with self.subTest(words=words):
                self.assertIn(
                    "link-vague", page_rules(page_html(body=f'<a href="x.html">{words}</a>'))
                )
        self.assertNotIn(
            "link-vague", page_rules(page_html(body='<a href="x.html">Read the manual</a>'))
        )

    def test_link_dup_name(self) -> None:
        differ = '<a href="a.html">Docs</a> <a href="b.html">Docs</a>'
        self.assertIn("link-dup-name", page_rules(page_html(body=differ)))
        same = '<a href="a.html">Docs</a> <a href="./a.html">Docs</a>'
        self.assertNotIn("link-dup-name", page_rules(page_html(body=same)))

    def test_button_name(self) -> None:
        self.assertIn("button-name", page_rules(page_html(body="<button></button>")))
        self.assertIn(
            "button-name", page_rules(page_html(body="<details><summary> </summary>x</details>"))
        )
        self.assertNotIn("button-name", page_rules(page_html(body="<button>Save</button>")))
        label = '<button aria-label="Close"></button>'
        self.assertNotIn("button-name", page_rules(page_html(body=label)))


class TableAndFormTests(unittest.TestCase):
    GOOD_TABLE = (
        "<table><caption>Sizes</caption><tr><th scope='col'>A</th></tr>"
        "<tr><td>1</td></tr></table>"
    )

    def test_table(self) -> None:
        self.assertNotIn("table", page_rules(page_html(body=self.GOOD_TABLE)))
        no_caption = self.GOOD_TABLE.replace("<caption>Sizes</caption>", "")
        self.assertIn("table", page_rules(page_html(body=no_caption)))
        labelled = no_caption.replace("<table>", '<table aria-label="Sizes">')
        self.assertNotIn("table", page_rules(page_html(body=labelled)))
        no_th = "<table><caption>x</caption><tr><td>1</td></tr></table>"
        self.assertIn("table", page_rules(page_html(body=no_th)))
        no_scope = self.GOOD_TABLE.replace(" scope='col'", "")
        self.assertIn("table", page_rules(page_html(body=no_scope)))

    def test_id_dup(self) -> None:
        self.assertIn("id-dup", page_rules(page_html(body='<p id="a">1</p><p id="a">2</p>')))
        self.assertNotIn("id-dup", page_rules(page_html(body='<p id="a">1</p><p id="b">2</p>')))

    def test_aria_ref(self) -> None:
        bad = '<p aria-describedby="ghost">x</p>'
        self.assertIn("aria-ref", page_rules(page_html(body=bad)))
        good = '<p id="d">x</p><p aria-describedby="d">y</p>'
        self.assertNotIn("aria-ref", page_rules(page_html(body=good)))
        label = '<label for="ghost">Name</label>'
        self.assertIn("aria-ref", page_rules(page_html(body=label)))

    def test_tabindex(self) -> None:
        self.assertIn("tabindex", page_rules(page_html(body='<div tabindex="1">x</div>')))
        self.assertNotIn("tabindex", page_rules(page_html(body='<div tabindex="0">x</div>')))
        self.assertNotIn("tabindex", page_rules(page_html(body='<div tabindex="-1">x</div>')))

    def test_form_label(self) -> None:
        self.assertIn("form-label", page_rules(page_html(body='<input type="text">')))
        self.assertIn("form-label", page_rules(page_html(body="<select></select>")))
        self.assertIn("form-label", page_rules(page_html(body="<textarea></textarea>")))
        by_for = '<label for="n">Name</label><input id="n">'
        self.assertNotIn("form-label", page_rules(page_html(body=by_for)))
        wrapped = "<label>Name <input></label>"
        self.assertNotIn("form-label", page_rules(page_html(body=wrapped)))
        aria = '<input aria-label="Name">'
        self.assertNotIn("form-label", page_rules(page_html(body=aria)))
        exempt = '<input type="hidden"><input type="submit" value="Go">'
        self.assertNotIn("form-label", page_rules(page_html(body=exempt)))


class PolicyTests(unittest.TestCase):
    def test_no_script(self) -> None:
        for snippet in (
            "<script>1</script>",
            '<script src="a.js"></script>',
            '<iframe src="a.html"></iframe>',
            '<object data="a.svg"></object>',
            '<embed src="a.svg">',
            '<button onclick="go()">Go</button>',
            '<p onmouseover="x()">hi</p>',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn("no-script", page_rules(page_html(body=snippet)))
        refresh = '<meta http-equiv="refresh" content="0; url=a.html">'
        self.assertIn("no-script", page_rules(page_html(head=refresh)))
        self.assertNotIn("no-script", page_rules(page_html()))

    def test_external_resource_in_markup(self) -> None:
        bad_snippets = {
            "img": '<img src="https://cdn.example/a.png" alt="">',
            "srcset": '<img src="a.png" srcset="b.png 1x, https://cdn.example/c.png 2x" alt="">',
            "protocol relative": '<img src="//cdn.example/a.png" alt="">',
            "video poster": '<video poster="https://cdn.example/p.png"></video>',
            "form": '<form action="https://forms.example/post"></form>',
            "style attr": '<p style="background:url(https://cdn.example/a.png)">x</p>',
            "style block": "<style>p{background:url('//cdn.example/a.png')}</style>",
            "import": "<style>@import 'other.css';</style>",
        }
        for label, snippet in bad_snippets.items():
            with self.subTest(label=label):
                self.assertIn("external-resource", page_rules(page_html(body=snippet)))
        for rel in ("stylesheet", "icon", "preload", "preconnect", "dns-prefetch", "manifest"):
            with self.subTest(rel=rel):
                link = f'<link rel="{rel}" href="https://cdn.example/x">'
                self.assertIn("external-resource", page_rules(page_html(head=link)))

    def test_external_resource_allowed_forms(self) -> None:
        canonical = '<link rel="canonical" href="https://example.org/">'
        self.assertNotIn("external-resource", page_rules(page_html(head=canonical)))
        inline = '<img src="data:image/png;base64,AAAA" alt="">'
        self.assertNotIn("external-resource", page_rules(page_html(body=inline)))
        style = "<style>p{background:url(data:image/png;base64,AAAA)}</style>"
        self.assertNotIn("external-resource", page_rules(page_html(body=style)))
        link = '<a href="https://github.com/x">Project</a>'
        self.assertNotIn("external-resource", page_rules(page_html(body=link)))

    def test_parse_problems_are_reported_not_raised(self) -> None:
        self.assertIn("parse", page_rules(page_html(body="<div><p>open")))
        self.assertIn("parse", page_rules(page_html(body="</section><p>stray</p>")))
        self.assertIn("parse", page_rules(page_html(body="<div><span></div>")))
        self.assertNotIn("parse", page_rules(page_html(body="<ul><li>a<li>b</ul><br/><p>x<p>y")))
        self.assertIsInstance(site_check.parse_page("<<<>>>&&& <a href"), site_check.Page)


class LinkTests(SiteCase):
    def with_body(self, body: str, extra: dict[str, str] | None = None) -> dict[str, str]:
        files = two_pages()
        files["index.html"] = page_html(body=body + '<a href="about.html">About us</a>')
        files.update(extra or {})
        return files

    def test_valid_site_has_no_problems(self) -> None:
        self.assertEqual(self.check(two_pages()), [])

    def test_link_broken(self) -> None:
        self.assertIn("link-broken", self.rules(self.with_body('<a href="missing.html">Gone</a>')))
        self.assertIn("link-broken", self.rules(self.with_body('<img src="img/no.png" alt="">')))
        self.assertIn("link-broken", self.rules(self.with_body('<a href="../x.html">Up</a>')))
        self.assertNotIn(
            "link-broken", self.rules(self.with_body('<a href="about.html">Again</a>'))
        )

    def test_link_broken_decodes_and_strips_query(self) -> None:
        extra = {"my file.pdf": "x"}
        body = '<a href="my%20file.pdf?v=2#page=1">Manual</a>'
        self.assertNotIn("link-broken", self.rules(self.with_body(body, extra)))

    def test_directory_link_needs_index(self) -> None:
        body = '<a href="guide/">Guide</a>'
        other = {"guide/page.html": "<p>No index here.</p>"}
        self.assertIn("link-broken", self.rules(self.with_body(body, other)))

    def test_directory_link_with_index(self) -> None:
        body = '<a href="guide/">Guide</a>'
        guide = page_html(title="Guide", nav='<a href="../index.html">Home</a>')
        extra = {"guide/index.html": guide.replace('"css/site.css"', '"../css/site.css"')}
        self.assertEqual(self.check(self.with_body(body, extra)), [])

    def test_link_absolute(self) -> None:
        self.assertIn("link-absolute", self.rules(self.with_body('<a href="/about.html">A</a>')))
        self.assertIn("link-absolute", self.rules(self.with_body('<img src="/img/a.png" alt="">')))

    def test_link_fragment(self) -> None:
        extra = {"about.html": page_html(title="About", body='<h2 id="team">Team</h2>')}
        good = '<a href="about.html#team">Team</a>'
        self.assertNotIn("link-fragment", self.rules(self.with_body(good, extra)))
        bad = '<a href="about.html#nothing">Nothing</a>'
        self.assertIn("link-fragment", self.rules(self.with_body(bad, extra)))
        self.assertIn("link-fragment", self.rules(self.with_body('<a href="#gone">Gone</a>')))
        self.assertNotIn("link-fragment", self.rules(self.with_body('<a href="#main">Main</a>')))

    def test_link_scheme(self) -> None:
        for href in ("http://example.org/", "javascript:void(0)", "data:text/plain,hi",
                     "//example.org/x", "tel:+441234"):
            with self.subTest(href=href):
                found = self.rules(self.with_body(f'<a href="{href}">Odd</a>'))
                self.assertIn("link-scheme", found)
        for href in ("https://github.com/x", "mailto:a@example.org"):
            with self.subTest(href=href):
                found = self.rules(self.with_body(f'<a href="{href}">Fine</a>'))
                self.assertNotIn("link-scheme", found)

    def test_link_host(self) -> None:
        config = {"allowed_link_hosts": ["github.com"]}
        ok = self.with_body('<a href="https://GitHub.com/x">Project</a>')
        self.assertNotIn("link-host", self.rules(ok, config))
        bad = self.with_body('<a href="https://evil.example/x">Elsewhere</a>')
        self.assertIn("link-host", self.rules(bad, config))
        self.assertNotIn("link-host", self.rules(bad, {}))

    def test_orphan(self) -> None:
        files = two_pages()
        files["lonely.html"] = page_html(title="Lonely", h1="Lonely")
        files["404.html"] = page_html(title="Not found", h1="Not found")
        found = self.check(files)
        self.assertEqual([(f, r) for f, r, _ in found], [("lonely.html", "orphan")])

    def test_external_paths_are_skipped(self) -> None:
        body = '<a href="docs/guide.html">Docs</a> <img src="docs/a.png" alt="">'
        files = self.with_body(body)
        self.assertIn("link-broken", self.rules(files))
        self.assertNotIn("link-broken", self.rules(files, {"external_paths": ["docs/"]}))

    def test_draft_pages(self) -> None:
        files = two_pages()
        config = {"draft_pages": ["about.html"]}
        self.assertIn("draft-missing", self.rules(files, config))
        files["about.html"] = page_html(title="About", body="<p>DRAFT: not final.</p>")
        self.assertNotIn("draft-missing", self.rules(files, config))
        self.assertIn("draft-missing", self.rules(files, {"draft_pages": ["ghost.html"]}))


class CssTests(SiteCase):
    def test_css_focus_visible_required(self) -> None:
        self.assertIn("css-focus", css_rules({"css/site.css": "a{color:red}"}))
        self.assertNotIn("css-focus", css_rules({"css/site.css": "a:focus-visible{color:red}"}))

    def test_css_focus_outline_removed(self) -> None:
        for declaration in ("outline:none", "outline : 0", "outline:0 !important", "outline:none;"):
            with self.subTest(declaration=declaration):
                css = f"a:focus-visible{{x:y}}a{{{declaration}}}"
                self.assertIn("css-focus", css_rules({"css/site.css": css}))
        fine = "a:focus-visible{outline:2px solid blue}"
        self.assertNotIn("css-focus", css_rules({"css/site.css": fine}))

    def test_css_px_font(self) -> None:
        base = "a:focus-visible{x:y}"
        self.assertIn("css-px-font", css_rules({"css/site.css": base + "p{font-size:14px}"}))
        self.assertIn(
            "css-px-font", css_rules({"css/site.css": base + "p{font-size:clamp(1rem,2vw,20px)}"})
        )
        for good in ("1rem", "1.2em", "120%", "larger", "clamp(1rem, 2vw, 2rem)"):
            with self.subTest(good=good):
                self.assertNotIn(
                    "css-px-font", css_rules({"css/site.css": base + f"p{{font-size:{good}}}"})
                )

    def test_css_motion(self) -> None:
        base = "a:focus-visible{x:y}"
        moving = base + "a{transition:color .2s}"
        self.assertIn("css-motion", css_rules({"css/site.css": moving}))
        self.assertIn("css-motion", css_rules({"css/site.css": base + "@keyframes k{}"}))
        calm = moving + "@media (prefers-reduced-motion:reduce){a{transition:none}}"
        self.assertNotIn("css-motion", css_rules({"css/site.css": calm}))
        self.assertNotIn("css-motion", css_rules({"css/site.css": base}))

    def test_css_literal_colour(self) -> None:
        self.assertEqual(site_check.literal_colour_problems(CSS), [])
        self.assertIn("css-literal-colour", css_rules({"css/site.css": CSS + "p{color:#fff}"}))
        self.assertIn("css-literal-colour", css_rules({"css/site.css": CSS + "p{color:#a1b2c3}"}))
        self.assertIn(
            "css-literal-colour", css_rules({"css/site.css": CSS + "p{color:rgb(1,2,3)}"})
        )
        self.assertIn(
            "css-literal-colour", css_rules({"css/site.css": CSS + "p{color:hsl(1 2% 3%)}"})
        )
        fine = CSS + "p{color:currentColor;background:transparent;border-color:var(--ink)}"
        fine += "a{color:color-mix(in srgb,var(--ink) 50%,var(--ground))}#add{margin:0}"
        self.assertNotIn("css-literal-colour", css_rules({"css/site.css": fine}))

    def test_css_literal_colour_only_checks_site_css(self) -> None:
        other = {"css/site.css": CSS, "css/print.css": "p{color:#fff}"}
        self.assertNotIn("css-literal-colour", css_rules(other))

    def test_css_urls(self) -> None:
        known = {"img/a.png", "css/site.css"}
        base = "a:focus-visible{x:y}"
        present = base + "p{background:url(../img/a.png)}"
        self.assertNotIn("link-broken", css_rules({"css/site.css": present}, known))
        absent = base + "p{background:url('../img/b.png')}"
        self.assertIn("link-broken", css_rules({"css/site.css": absent}, known))
        self.assertIn(
            "external-resource",
            css_rules({"css/site.css": base + "p{background:url(https://x.example/a.png)}"}, known),
        )
        imported = base + "@import url(other.css);"
        self.assertIn("external-resource", css_rules({"css/site.css": imported}, known))
        self.assertIn(
            "external-resource", css_rules({"css/site.css": base + '@import "other.css";'}, known)
        )
        data = base + "p{background:url(data:image/svg+xml;utf8,%3Csvg%3E)}"
        self.assertNotIn("external-resource", css_rules({"css/site.css": data}, known))

    def test_css_checks_run_on_site_files(self) -> None:
        files = two_pages()
        files["css/site.css"] = "a{outline:none;color:#fff}"
        found = {(f, r) for f, r, _ in self.check(files)}
        self.assertIn(("css/site.css", "css-focus"), found)
        self.assertIn(("css/site.css", "css-literal-colour"), found)


class TokensAndExternalTests(SiteCase):
    def test_contrast_through_the_tree(self) -> None:
        tokens = self.root.parent / "tokens.json"
        tokens.write_text(json.dumps(TOKENS), encoding="utf-8")
        passing = {"contrast_pairs": [["ink", "ground", 4.5]]}
        self.assertEqual(self.check(two_pages(), passing, tokens_path=tokens), [])
        failing = {"contrast_pairs": [["pale", "ground", 3.0]]}
        found = site_check.check_site(self.root, failing, tokens).problems
        self.assertTrue(found)
        self.assertEqual({(f, r) for f, r, _ in found}, {("tokens", "contrast")})

    def test_unreadable_tokens_file_is_a_problem(self) -> None:
        config = {"contrast_pairs": [["ink", "ground", 4.5]]}
        found = self.check(two_pages(), config, tokens_path=self.root.parent / "missing.json")
        self.assertEqual({(f, r) for f, r, _ in found}, {("tokens", "contrast")})

    def test_external_uses_the_fetch_function(self) -> None:
        body = (
            '<a href="https://good.example/a#frag">Good</a> '
            '<a href="https://good.example/a">Good again</a> '
            '<a href="https://gone.example/">Gone</a> '
            '<a href="https://down.example/">Down</a>'
        )
        files = two_pages()
        files["index.html"] = page_html(body=body + '<a href="about.html">About us</a>')
        calls: list[str] = []

        def fake(url: str) -> int:
            calls.append(url)
            if "down" in url:
                raise OSError("connection refused")
            return 404 if "gone" in url else 200

        found = self.check(files, external=True, fetch=fake)
        self.assertEqual({rule for _, rule, _ in found}, {"link-external"})
        self.assertEqual(sorted(calls), sorted(set(calls)))
        self.assertEqual(len(calls), 3)
        text = " ".join(message for _, _, message in found)
        self.assertIn("404", text)
        self.assertIn("connection refused", text)
        self.assertNotIn("good.example", text)

    def test_no_fetch_without_the_flag(self) -> None:
        files = two_pages()
        files["index.html"] = page_html(
            body='<a href="https://x.example/">X</a><a href="about.html">About us</a>'
        )

        def forbidden(url: str) -> int:
            raise AssertionError("the network must not be used")

        self.assertEqual(self.check(files, fetch=forbidden), [])

    def test_fetch_failure_does_not_stop_the_run(self) -> None:
        results = site_check.check_external({"a.html": {"https://x.example/"}}, lambda u: 500)
        self.assertEqual(results[0][1], "link-external")


class CommandLineTests(SiteCase):
    def run_main(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = site_check.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def test_valid_site_exits_zero(self) -> None:
        self.write(two_pages())
        config = self.root.parent / "site.json"
        config.write_text(json.dumps({"allowed_link_hosts": ["github.com"]}), encoding="utf-8")
        code, out, _ = self.run_main(str(self.root), "--config", str(config))
        self.assertEqual(code, 0, out)
        self.assertEqual(
            out.strip(),
            "site_check: 2 pages, 5 internal links, 0 external links, all checks passed",
        )

    def test_problems_exit_one_with_sorted_lines(self) -> None:
        files = two_pages()
        files["about.html"] = page_html(title="About", body='<img src="a.png">', html_attrs="")
        self.write(files)
        config = self.root.parent / "site.json"
        config.write_text("{}", encoding="utf-8")
        code, out, _ = self.run_main(str(self.root), "--config", str(config))
        lines = out.strip().splitlines()
        self.assertEqual(code, 1)
        self.assertEqual(lines[:-1], sorted(lines[:-1]))
        self.assertTrue(lines[0].startswith("about.html: "))
        self.assertIn("about.html: img-alt: ", out)
        self.assertIn("about.html: lang: ", out)
        self.assertRegex(lines[-1], r"^site_check: \d+ problems in 2 pages$")

    def test_missing_directory_exits_two(self) -> None:
        code, out, err = self.run_main(str(self.root / "nope"))
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("not a directory", err)

    def test_directory_without_pages_exits_two(self) -> None:
        self.write({"css/site.css": CSS})
        code, _, err = self.run_main(str(self.root))
        self.assertEqual(code, 2)
        self.assertIn("no HTML page", err)

    def test_missing_explicit_config_exits_two(self) -> None:
        self.write(two_pages())
        code, _, err = self.run_main(str(self.root), "--config", str(self.root / "none.json"))
        self.assertEqual(code, 2)
        self.assertIn("does not exist", err)

    def test_external_flag_uses_replaceable_fetch(self) -> None:
        files = two_pages()
        files["index.html"] = page_html(
            body='<a href="https://x.example/">X</a><a href="about.html">About us</a>'
        )
        self.write(files)
        config = self.root.parent / "site.json"
        config.write_text("{}", encoding="utf-8")
        with mock.patch.object(site_check, "fetch_url", return_value=503) as fetch:
            code, out, _ = self.run_main(str(self.root), "--config", str(config), "--external")
        self.assertEqual(code, 1)
        fetch.assert_called_once_with("https://x.example/")
        self.assertIn("index.html: link-external: https://x.example/ returned status 503", out)

    def test_default_directory_is_under_the_repository(self) -> None:
        self.assertEqual(
            site_check.DEFAULT_SITE_DIR, site_check.REPO_ROOT / "build" / "website"
        )


if __name__ == "__main__":
    unittest.main()
