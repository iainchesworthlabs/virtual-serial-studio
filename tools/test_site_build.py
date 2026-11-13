#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/site_build.py.

Run with:

    python -m unittest discover -s tools -p "test_site_build.py" -v

Every test builds a small site in a temporary directory.  Nothing here uses the
network: the release refresh is exercised with a fake fetch function and with a
patched ``urlopen``.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import site_build  # noqa: E402  (needs the path tweak above)

BASE_URL = "https://example.test/"
REPO = "example/site"
RELEASE_PREFIX = f"https://github.com/{REPO}/releases/"

TEMPLATE = """<!doctype html>
<title>{{head_title}}</title>
<meta name="description" content="{{description}}">
<link rel="canonical" href="{{canonical}}">
<nav><ul>
{{nav}}
</ul></nav>
<main>
{{content}}
</main>
<footer>{{name}} {{release_tag}} {{repo_url}}</footer>
"""

TOKENS = {
    "color": {
        "light": {"ground": "#ECEEF1", "accentStrong": "#1A4FB0", "line2": "#C0C4CC",
                  "noteWarnText": "#7A4B00"},
        "dark": {"ground": "#0E0F12", "accentStrong": "#7AA2F7", "line2": "#2A2D34",
                 "noteWarnText": "#F2C14E"},
    },
    "space": {"one": 4},
}

SITE = {
    "name": "Example Studio",
    "base_url": BASE_URL,
    "repo": REPO,
    "tokens": "tokens.json",
    "nav": [
        {"file": "features.html", "label": "Features"},
        {"file": "download.html", "label": "Download"},
    ],
    "copy": [{"from": "assets/icon.svg", "to": "img/icon.svg"}],
    "download_rules": [
        {"match": "-windows-x86_64-testsigned\\.exe$", "platform": "Windows",
         "label": "Installer, with the test-signed driver"},
        {"match": "-windows-x86_64\\.exe$", "platform": "Windows", "label": "Installer"},
        {"match": "\\.deb$", "platform": "Linux", "label": "Debian package"},
        {"match": "\\.rpm$", "platform": "Linux", "label": "RPM package"},
    ],
}

RELEASE = {
    "tag": "v0.1.0-rc.3",
    "prerelease": True,
    "published_at": "2026-11-12T11:18:55Z",
    "html_url": RELEASE_PREFIX + "tag/v0.1.0-rc.3",
    "assets": [],
}


def page(title: str = "Features", description: str = "One sentence.",
         body: str = "<section>Body</section>\n", **extra: str) -> str:
    lines = [f"title: {title}", f"description: {description}"]
    lines += [f"{key}: {value}" for key, value in extra.items()]
    return "<!--\n" + "\n".join(lines) + "\n-->\n" + body


def asset(name: str, size: int = 1000) -> dict[str, object]:
    return {"name": name, "size": size, "url": RELEASE_PREFIX + "download/v0.1.0-rc.3/" + name}


def write(path: Path, text: str | bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(text, str):
        text = text.encode("utf-8")
    path.write_bytes(text)


class SiteTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve() / "repo"
        self.source = self.root / "website"
        self.out = self.root / "build" / "website"
        write(self.root / "tokens.json", json.dumps(TOKENS))
        write(self.root / "assets" / "icon.svg", "<svg/>\n")
        write(self.source / "site.json", json.dumps(SITE))
        write(self.source / "templates" / "base.html", TEMPLATE)
        write(self.source / "data" / "release.json", json.dumps(RELEASE))
        write(self.source / "static" / "css" / "site.css", "body{margin:0}\n")
        write(self.source / "static" / "robots.txt", "User-agent: *\n")
        write(self.source / "pages" / "index.html",
              page("Home", "The home page.", nav="features.html"))
        write(self.source / "pages" / "features.html", page())
        write(self.source / "pages" / "download.html",
              page("Download", "Get it.", "{{downloads}}\n"))

    def config(self, **changes: object) -> None:
        write(self.source / "site.json", json.dumps({**SITE, **changes}))

    def release(self, **changes: object) -> None:
        write(self.source / "data" / "release.json", json.dumps({**RELEASE, **changes}))

    def put_page(self, name: str, text: str) -> None:
        write(self.source / "pages" / name, text)

    def build(self) -> int:
        return site_build.build_site(self.root, self.out)

    def output(self, name: str) -> str:
        return (self.out / name).read_text(encoding="utf-8")

    def snapshot(self) -> dict[str, bytes]:
        return {p.relative_to(self.out).as_posix(): p.read_bytes()
                for p in sorted(self.out.rglob("*")) if p.is_file()}

    def assertBuildError(self, *fragments: str) -> None:
        with self.assertRaises(site_build.BuildError) as caught:
            self.build()
        for fragment in fragments:
            self.assertIn(fragment, str(caught.exception))


class FrontMatterTests(unittest.TestCase):
    def test_parses_all_keys_and_strips_the_comment(self) -> None:
        text = page("A: B", "Why: because.", "<p>x</p>", nav="download.html", sitemap="no")
        parsed = site_build.parse_page("a.html", text)
        self.assertEqual(parsed.title, "A: B")
        self.assertEqual(parsed.description, "Why: because.")
        self.assertEqual(parsed.nav, "download.html")
        self.assertFalse(parsed.sitemap)
        self.assertEqual(parsed.body, "<p>x</p>\n")

    def test_defaults(self) -> None:
        parsed = site_build.parse_page("a.html", page())
        self.assertIsNone(parsed.nav)
        self.assertTrue(parsed.sitemap)

    def test_crlf_input_is_normalised(self) -> None:
        parsed = site_build.parse_page("a.html", page(body="<p>x</p>\n").replace("\n", "\r\n"))
        self.assertNotIn("\r", parsed.body)
        self.assertEqual(parsed.title, "Features")

    def test_errors(self) -> None:
        cases = {
            "missing comment": ("<p>no front matter</p>", "front-matter comment"),
            "unterminated": ("<!--\ntitle: A\n", "not closed"),
            "missing title": ("<!--\ndescription: d\n-->\n", "'title' is required"),
            "empty description": ("<!--\ntitle: t\ndescription:   \n-->\n", "'description'"),
            "unknown key": (page(colour="red"), "unknown front-matter key 'colour'"),
            "duplicate key": ("<!--\ntitle: a\ntitle: b\ndescription: d\n-->\n", "appears twice"),
            "no colon": ("<!--\ntitle a\ndescription: d\n-->\n", "not 'key: value'"),
            "bad sitemap": (page(sitemap="maybe"), "'sitemap' must be 'yes' or 'no'"),
            "empty nav": ("<!--\ntitle: t\ndescription: d\nnav:\n-->\n", "'nav' must not be empty"),
        }
        for label, (text, message) in cases.items():
            with self.subTest(label):
                with self.assertRaises(site_build.BuildError) as caught:
                    site_build.parse_page("a.html", text)
                self.assertIn("a.html", str(caught.exception))
                self.assertIn(message, str(caught.exception))


class FormattingTests(unittest.TestCase):
    def test_sizes(self) -> None:
        cases = {
            0: "0 kB", 1: "1 kB", 500: "1 kB", 1000: "1 kB", 268_000: "268 kB",
            268_400: "268 kB", 999_999: "1000 kB", 1_000_000: "1.0 MB",
            43_949_999: "43.9 MB", 43_950_000: "44.0 MB", 43_900_000: "43.9 MB",
            1_234_567_890: "1234.6 MB",
        }
        for size, expected in cases.items():
            with self.subTest(size=size):
                self.assertEqual(site_build.format_size(size), expected)

    def test_dates(self) -> None:
        self.assertEqual(site_build.format_date("2026-11-12T11:18:55Z"), "12 November 2026")
        self.assertEqual(site_build.format_date("2026-01-05T00:00:00Z"), "5 January 2026")
        self.assertEqual(site_build.format_date("2027-12-31"), "31 December 2027")

    def test_bad_dates(self) -> None:
        for text in ("yesterday", "2026-13-01T00:00:00Z", "2026-00-10T00:00:00Z"):
            with self.subTest(text), self.assertRaises(site_build.BuildError):
                site_build.format_date(text)

    def test_kebab_case_and_platform_id(self) -> None:
        self.assertEqual(site_build.kebab_case("accentStrong"), "accent-strong")
        self.assertEqual(site_build.kebab_case("line2"), "line2")
        self.assertEqual(site_build.kebab_case("noteWarnText"), "note-warn-text")
        self.assertEqual(site_build.kebab_case("ground"), "ground")
        self.assertEqual(site_build.platform_id("Other files"), "dl-other-files")
        self.assertEqual(site_build.platform_id("macOS (Apple silicon)"), "dl-macos-apple-silicon")


class TokenTests(unittest.TestCase):
    def test_generated_block(self) -> None:
        block = site_build.render_tokens_css(TOKENS, "tokens.json")
        self.assertEqual(
            block,
            "/* Generated by tools/site_build.py from tokens.json. Do not edit. */\n"
            ":root{color-scheme:light dark;--ground:#ECEEF1;--accent-strong:#1A4FB0;"
            "--line2:#C0C4CC;--note-warn-text:#7A4B00}\n"
            "@media (prefers-color-scheme:dark){:root{--ground:#0E0F12;--accent-strong:#7AA2F7;"
            "--line2:#2A2D34;--note-warn-text:#F2C14E}}",
        )

    def test_order_follows_the_json(self) -> None:
        tokens = {"color": {"light": {"zeta": "#000000", "alpha": "#FFFFFF"},
                            "dark": {"zeta": "#111111", "alpha": "#EEEEEE"}}}
        block = site_build.render_tokens_css(tokens, "t.json")
        self.assertIn("--zeta:#000000;--alpha:#FFFFFF}", block)

    def test_bad_colours(self) -> None:
        for value in ("#FFF", "red", "#12345G", "#1234567", 12, "rgb(0,0,0)"):
            tokens = {"color": {"light": {"ground": value}, "dark": {"ground": "#000000"}}}
            with self.subTest(value=value), self.assertRaises(site_build.BuildError) as caught:
                site_build.render_tokens_css(tokens, "t.json")
            self.assertIn("light.ground", str(caught.exception))

    def test_missing_scheme(self) -> None:
        with self.assertRaises(site_build.BuildError):
            site_build.render_tokens_css({"color": {"light": {}}}, "t.json")


class DownloadTests(unittest.TestCase):
    @staticmethod
    def render(assets: list[dict[str, object]]) -> str:
        release = site_build.Release(
            "v1", False, "2026-01-01T00:00:00Z", RELEASE_PREFIX + "tag/v1",
            tuple(site_build.Asset(str(a["name"]), int(str(a["size"])), str(a["url"]))
                  for a in assets),
        )
        rules = tuple(
            site_build.DownloadRule(__import__("re").compile(r["match"]), r["platform"], r["label"])
            for r in SITE["download_rules"]
        )
        return site_build.render_downloads(release, rules)

    def test_exact_markup_with_signature(self) -> None:
        name = "vss-0.1.0-windows-x86_64-testsigned.exe"
        out = self.render([asset(name, 43_900_000), asset(name + ".asc", 800)])
        url = RELEASE_PREFIX + "download/v0.1.0-rc.3/" + name
        self.assertEqual(
            out,
            '<section class="dl-group" aria-labelledby="dl-windows">\n'
            '<h2 id="dl-windows">Windows</h2>\n'
            '<table class="dl">\n'
            '<caption class="visually-hidden">Windows downloads</caption>\n'
            '<thead><tr><th scope="col">File</th><th scope="col">Size</th>'
            '<th scope="col">Signature</th></tr></thead>\n'
            "<tbody>\n"
            f'<tr><th scope="row"><a href="{url}">Installer, with the test-signed driver</a>'
            f'<span class="dl-name">{name}</span></th><td>43.9 MB</td>'
            f'<td><a href="{url}.asc" aria-label="Signature for {name}">.asc</a></td></tr>\n'
            "</tbody>\n</table>\n</section>",
        )

    def test_no_signature_cell(self) -> None:
        out = self.render([asset("vss_1_amd64.deb", 268_000)])
        self.assertIn("<td>268 kB</td><td>None</td></tr>", out)
        self.assertNotIn(".asc", out)

    def test_signature_is_not_a_row(self) -> None:
        out = self.render([asset("a.deb"), asset("a.deb.asc")])
        self.assertEqual(out.count("<tr><th scope=\"row\">"), 1)

    def test_orphan_signature_is_listed_as_an_ordinary_file(self) -> None:
        out = self.render([asset("lonely.deb.asc")])
        self.assertIn('<h2 id="dl-other-files">Other files</h2>', out)
        self.assertIn('<span class="dl-name">lonely.deb.asc</span>', out)

    def test_group_and_row_order(self) -> None:
        out = self.render([
            asset("zzz.txt"), asset("b.rpm"), asset("a.rpm"), asset("c.deb"),
            asset("x-windows-x86_64.exe"), asset("x-windows-x86_64-testsigned.exe"),
            asset("aaa.txt"),
        ])
        headings = [line for line in out.splitlines() if line.startswith("<h2")]
        self.assertEqual(headings, [
            '<h2 id="dl-windows">Windows</h2>',
            '<h2 id="dl-linux">Linux</h2>',
            '<h2 id="dl-other-files">Other files</h2>',
        ])
        names = [part.split("<")[0] for part in out.split('<span class="dl-name">')[1:]]
        self.assertEqual(names, [
            "x-windows-x86_64-testsigned.exe", "x-windows-x86_64.exe",
            "c.deb", "a.rpm", "b.rpm",
            "aaa.txt", "zzz.txt",
        ])

    def test_other_files_use_the_file_name_as_label(self) -> None:
        out = self.render([asset("notes.txt")])
        self.assertIn(">notes.txt</a><span class=\"dl-name\">notes.txt</span>", out)

    def test_empty_groups_are_skipped(self) -> None:
        out = self.render([asset("a.deb")])
        self.assertNotIn("Windows", out)
        self.assertNotIn("Other files", out)

    def test_escaping(self) -> None:
        out = self.render([asset('a"b&c.deb')])
        self.assertIn("a&quot;b&amp;c.deb", out)
        self.assertNotIn('a"b', out)


class PageBuildTests(SiteTestCase):
    def test_build_writes_pages_and_reports_the_count(self) -> None:
        self.assertEqual(self.build(), 3)
        for name in ("index.html", "features.html", "download.html", "sitemap.xml",
                     "css/site.css", "robots.txt", "img/icon.svg"):
            self.assertTrue((self.out / name).is_file(), name)

    def test_head_title_and_canonical(self) -> None:
        self.build()
        home = self.output("index.html")
        self.assertIn("<title>Home</title>", home)
        self.assertIn(f'<link rel="canonical" href="{BASE_URL}">', home)
        features = self.output("features.html")
        self.assertIn("<title>Features | Example Studio</title>", features)
        self.assertIn(f'<link rel="canonical" href="{BASE_URL}features.html">', features)

    def test_simple_keys(self) -> None:
        self.build()
        features = self.output("features.html")
        self.assertIn('<meta name="description" content="One sentence.">', features)
        self.assertIn("<footer>Example Studio v0.1.0-rc.3 https://github.com/example/site</footer>",
                      features)
        self.assertIn("<section>Body</section>", features)
        self.assertNotIn("<!--", features)

    def test_release_keys(self) -> None:
        self.put_page("release.html", page(
            "R", "d", "{{release_tag}}|{{release_version}}|{{release_url}}|{{release_kind}}|"
                      "{{release_date}}\n"))
        self.build()
        self.assertIn(f"v0.1.0-rc.3|0.1.0-rc.3|{RELEASE_PREFIX}tag/v0.1.0-rc.3|Pre-release|"
                      "12 November 2026", self.output("release.html"))
        self.release(prerelease=False, tag="v1.0.0")
        self.build()
        self.assertIn("|1.0.0|", self.output("release.html"))
        self.assertIn("|Release|", self.output("release.html"))

    def test_data_values_are_escaped_but_html_keys_are_not(self) -> None:
        self.put_page("odd.html", page('A "quote" & <b>', "x < y & \"z\"", "<p>{{title}}</p>\n"))
        self.config(name="Fish & Chips", nav=[{"file": "odd.html", "label": "<Odd>"},
                                              {"file": "features.html", "label": "F"}])
        self.build()
        text = self.output("odd.html")
        self.assertIn("<p>A &quot;quote&quot; &amp; &lt;b&gt;</p>", text)
        self.assertIn('content="x &lt; y &amp; &quot;z&quot;"', text)
        self.assertIn("<title>A &quot;quote&quot; &amp; &lt;b&gt; | Fish &amp; Chips</title>", text)
        self.assertIn('<li><a href="odd.html" aria-current="page">&lt;Odd&gt;</a></li>', text)

    def test_unknown_placeholder_names_file_and_key(self) -> None:
        self.put_page("bad.html", page(body="<p>{{nonsense}}</p>\n"))
        self.assertBuildError("bad.html", "{{nonsense}}")

    def test_unknown_placeholder_in_template(self) -> None:
        write(self.source / "templates" / "base.html", TEMPLATE + "{{mystery}}\n")
        self.assertBuildError("base.html", "{{mystery}}")

    def test_content_is_not_allowed_in_a_fragment(self) -> None:
        self.put_page("bad.html", page(body="{{content}}\n"))
        self.assertBuildError("bad.html", "{{content}}")

    def test_template_needs_content(self) -> None:
        write(self.source / "templates" / "base.html", "<p>{{name}}</p>\n")
        self.assertBuildError("{{content}}")

    def test_placeholder_left_by_a_data_value_is_an_error(self) -> None:
        self.put_page("sneaky.html", page("Sneaky {{name}}", "d"))
        self.assertBuildError("sneaky.html", "{{name}}")

    def test_non_placeholders_are_left_alone(self) -> None:
        self.put_page("braces.html", page(body="<p>{{ spaced }} {{Upper}} {single}</p>\n"))
        self.build()
        self.assertIn("{{ spaced }} {{Upper}} {single}", self.output("braces.html"))

    def test_nav_marks_the_current_page(self) -> None:
        self.build()
        features = self.output("features.html")
        self.assertIn('<li><a href="features.html" aria-current="page">Features</a></li>\n'
                      '<li><a href="download.html">Download</a></li>', features)
        self.assertEqual(features.count("aria-current"), 1)

    def test_nav_override_and_unlisted_page(self) -> None:
        self.build()
        home = self.output("index.html")  # nav: features.html
        self.assertIn('<a href="features.html" aria-current="page">', home)
        self.put_page("legal.html", page("Legal", "d"))
        self.build()
        self.assertNotIn("aria-current", self.output("legal.html"))

    def test_bad_nav_value(self) -> None:
        self.put_page("odd.html", page(nav="nowhere.html"))
        self.assertBuildError("odd.html", "nowhere.html")

    def test_page_errors_are_reported_with_the_file(self) -> None:
        self.put_page("broken.html", "<p>no front matter</p>")
        self.assertBuildError("broken.html")

    def test_missing_inputs(self) -> None:
        (self.source / "data" / "release.json").unlink()
        self.assertBuildError("release.json")

    def test_downloads_in_a_page(self) -> None:
        self.release(assets=[asset("p.deb", 2_000_000), asset("p.deb.asc", 800)])
        self.build()
        text = self.output("download.html")
        self.assertIn('<h2 id="dl-linux">Linux</h2>', text)
        self.assertIn("<td>2.0 MB</td>", text)


class OutputTests(SiteTestCase):
    def test_css_is_tokens_then_site_css(self) -> None:
        self.build()
        css = self.output("css/site.css")
        block = site_build.render_tokens_css(TOKENS, "tokens.json")
        self.assertEqual(css, block + "\nbody{margin:0}\n")
        self.assertTrue(css.startswith("/* Generated by tools/site_build.py from tokens.json."))

    def test_bad_token_colour_fails_the_build(self) -> None:
        bad = json.loads(json.dumps(TOKENS))
        bad["color"]["dark"]["ground"] = "black"
        write(self.root / "tokens.json", json.dumps(bad))
        self.assertBuildError("dark.ground")

    def test_static_files_keep_their_paths_and_bytes(self) -> None:
        payload = bytes(range(256)) + b"\r\n"
        write(self.source / "static" / "img" / "deep" / "pic.bin", payload)
        self.build()
        self.assertEqual((self.out / "img" / "deep" / "pic.bin").read_bytes(), payload)
        self.assertEqual(self.output("robots.txt"), "User-agent: *\n")

    def test_static_site_css_is_not_copied_verbatim(self) -> None:
        self.build()
        self.assertNotEqual(self.output("css/site.css"), "body{margin:0}\n")
        self.assertIn("--ground:#ECEEF1", self.output("css/site.css"))

    def test_missing_site_css(self) -> None:
        (self.source / "static" / "css" / "site.css").unlink()
        self.assertBuildError("site.css")

    def test_copy_entries(self) -> None:
        self.build()
        self.assertEqual(self.output("img/icon.svg"), "<svg/>\n")

    def test_copy_missing_source(self) -> None:
        (self.root / "assets" / "icon.svg").unlink()
        self.assertBuildError("assets/icon.svg")

    def test_copy_target_must_stay_inside_the_output(self) -> None:
        for target in ("../escape.svg", "/abs.svg", "a/../../b.svg", "C:/x.svg", "a\\b.svg", ""):
            with self.subTest(target=target):
                self.config(copy=[{"from": "assets/icon.svg", "to": target or "x"}]
                            if target else [{"from": "assets/icon.svg", "to": "."}])
                self.assertBuildError("copy target")

    def test_duplicate_outputs_are_an_error(self) -> None:
        write(self.source / "static" / "features.html", "x")
        self.assertBuildError("features.html", "twice")

    def test_stale_files_are_removed(self) -> None:
        write(self.out / "stale.txt", "old")
        self.build()
        self.assertFalse((self.out / "stale.txt").exists())

    def test_sitemap(self) -> None:
        self.put_page("legal.html", page("Legal", "d", sitemap="no"))
        self.put_page("zebra.html", page("Zebra", "d"))
        self.build()
        self.assertEqual(
            self.output("sitemap.xml"),
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
            f"<url><loc>{BASE_URL}</loc></url>\n"
            f"<url><loc>{BASE_URL}download.html</loc></url>\n"
            f"<url><loc>{BASE_URL}features.html</loc></url>\n"
            f"<url><loc>{BASE_URL}zebra.html</loc></url>\n"
            "</urlset>\n",
        )
        self.assertNotIn("legal.html", self.output("sitemap.xml"))
        self.assertNotIn("lastmod", self.output("sitemap.xml"))

    def test_output_is_lf_utf8(self) -> None:
        text = page("Caf\u00e9", "d", "<p>\u00e9\u2014</p>\n")
        self.put_page("unicode.html", text.replace("\n", "\r\n"))
        write(self.source / "static" / "css" / "site.css", "a{b:c}\r\nd{e:f}\r\n")
        self.build()
        for name, data in self.snapshot().items():
            if name.endswith((".html", ".css", ".xml")):
                self.assertNotIn(b"\r", data, name)
        self.assertIn("Caf\u00e9", self.output("unicode.html"))

    def test_two_builds_are_identical(self) -> None:
        self.release(assets=[asset("b.deb"), asset("a.deb"), asset("a.deb.asc")])
        self.build()
        first = self.snapshot()
        other = self.root / "second"
        site_build.build_site(self.root, other)
        second = {p.relative_to(other).as_posix(): p.read_bytes()
                  for p in sorted(other.rglob("*")) if p.is_file()}
        self.assertEqual(first, second)
        self.build()
        self.assertEqual(first, self.snapshot())


class DangerousOutTests(SiteTestCase):
    def test_refuses_root_source_and_ancestors(self) -> None:
        for out in (self.root, self.source, self.root.parent, Path(self.root.anchor)):
            with self.subTest(out=out), self.assertRaises(site_build.BuildError) as caught:
                site_build.build_site(self.root, out)
            self.assertIn("refusing", str(caught.exception))
        self.assertTrue((self.source / "site.json").is_file())

    def test_refuses_a_directory_that_holds_inputs(self) -> None:
        with self.assertRaises(site_build.BuildError):
            site_build.build_site(self.root, self.source / "static")
        self.assertTrue((self.source / "static" / "robots.txt").is_file())
        with self.assertRaises(site_build.BuildError):
            site_build.build_site(self.root, self.root / "assets")
        self.assertTrue((self.root / "assets" / "icon.svg").is_file())

    def test_refuses_a_file(self) -> None:
        target = self.root / "afile"
        write(target, "x")
        with self.assertRaises(site_build.BuildError):
            site_build.build_site(self.root, target)

    def test_main_refuses_and_returns_one(self) -> None:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = site_build.main(["--root", str(self.root), "--out", str(self.root)])
        self.assertEqual(code, 1)
        self.assertTrue(err.getvalue().startswith("site_build: "))


class MainTests(SiteTestCase):
    def test_success_message_and_default_out(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = site_build.main(["--root", str(self.root)])
        self.assertEqual(code, 0)
        self.assertEqual(out.getvalue(), f"site_build: wrote 3 pages to {self.out}\n")
        self.assertTrue((self.out / "index.html").is_file())

    def test_build_error_goes_to_stderr(self) -> None:
        self.put_page("bad.html", page(body="{{nope}}\n"))
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            code = site_build.main(["--root", str(self.root)])
        self.assertEqual(code, 1)
        self.assertIn("site_build: pages/bad.html: unknown placeholder {{nope}}", err.getvalue())

    def test_refresh_flag_uses_the_replaceable_fetch(self) -> None:
        api = [{
            "tag_name": "v9.9.9", "prerelease": False, "published_at": "2027-01-02T03:04:05Z",
            "html_url": RELEASE_PREFIX + "tag/v9.9.9", "assets": [],
        }]
        with mock.patch.object(site_build, "fetch_releases", return_value=api) as fake:
            with contextlib.redirect_stdout(io.StringIO()):
                code = site_build.main(["--root", str(self.root), "--refresh-release"])
        self.assertEqual(code, 0)
        fake.assert_called_once_with(REPO)
        self.assertIn("v9.9.9", self.output("features.html"))


class RefreshTests(SiteTestCase):
    @staticmethod
    def api_release(**changes: object) -> dict[str, object]:
        entry: dict[str, object] = {
            "tag_name": "v0.2.0",
            "prerelease": False,
            "published_at": "2026-12-01T10:00:00Z",
            "html_url": RELEASE_PREFIX + "tag/v0.2.0",
            "assets": [
                {"name": "b.deb", "size": 20, "id": 2, "content_type": "x",
                 "browser_download_url": RELEASE_PREFIX + "download/v0.2.0/b.deb"},
                {"name": "a.deb", "size": 10,
                 "browser_download_url": RELEASE_PREFIX + "download/v0.2.0/a.deb"},
            ],
            "body": "ignored",
        }
        entry.update(changes)
        return entry

    def refresh(self, response: object) -> Path:
        seen: list[str] = []

        def fake(repo: str) -> object:
            seen.append(repo)
            return response

        path = site_build.refresh_release(self.root, fetch=fake)
        self.assertEqual(seen, [REPO])
        return path

    def test_writes_the_expected_shape(self) -> None:
        path = self.refresh([self.api_release(), self.api_release(tag_name="v0.1.0")])
        text = path.read_text(encoding="utf-8")
        self.assertEqual(path, self.source.resolve() / "data" / "release.json")
        self.assertEqual(
            text,
            json.dumps({
                "tag": "v0.2.0", "prerelease": False, "published_at": "2026-12-01T10:00:00Z",
                "html_url": RELEASE_PREFIX + "tag/v0.2.0",
                "assets": [
                    {"name": "a.deb", "size": 10, "url": RELEASE_PREFIX + "download/v0.2.0/a.deb"},
                    {"name": "b.deb", "size": 20, "url": RELEASE_PREFIX + "download/v0.2.0/b.deb"},
                ],
            }, indent=1) + "\n",
        )
        self.assertNotIn(b"\r", path.read_bytes())

    def test_refreshed_file_builds(self) -> None:
        self.refresh([self.api_release()])
        self.build()
        self.assertIn("v0.2.0", self.output("features.html"))
        self.assertIn("a.deb", self.output("download.html"))

    def test_foreign_urls_are_rejected(self) -> None:
        foreign = "https://evil.test/releases/download/x.deb"
        cases = {
            "asset": self.api_release(assets=[{"name": "x.deb", "size": 1,
                                               "browser_download_url": foreign}]),
            "html_url": self.api_release(html_url="https://github.com/other/repo/releases/tag/v1"),
            "look-alike": self.api_release(
                html_url="https://github.com/example/site-evil/releases/tag/v1"),
            "scheme": self.api_release(html_url="http://github.com/example/site/releases/tag/v1"),
        }
        for label, entry in cases.items():
            with self.subTest(label):
                before = (self.source / "data" / "release.json").read_bytes()
                with self.assertRaises(site_build.BuildError) as caught:
                    self.refresh([entry])
                self.assertIn(RELEASE_PREFIX, str(caught.exception))
                self.assertEqual((self.source / "data" / "release.json").read_bytes(), before)

    def test_empty_and_malformed_responses(self) -> None:
        for response in ([], {}, None, [None], [{"tag_name": "v1"}]):
            with self.subTest(response=response), self.assertRaises(site_build.BuildError):
                self.refresh(response)

    def test_network_failure_is_a_build_error(self) -> None:
        def broken(repo: str) -> object:
            raise OSError("no route to host")

        with self.assertRaises(site_build.BuildError) as caught:
            site_build.refresh_release(self.root, fetch=broken)
        self.assertIn("no route to host", str(caught.exception))
        self.assertTrue((self.source / "data" / "release.json").is_file())

    def test_default_fetch_sends_the_documented_request(self) -> None:
        captured = {}

        class Response(io.BytesIO):
            def __enter__(self) -> "Response":
                return self

            def __exit__(self, *exc: object) -> None:
                self.close()

        def fake_urlopen(request, timeout):  # type: ignore[no-untyped-def]
            captured["request"] = request
            captured["timeout"] = timeout
            return Response(b"[]")

        for token in ("s3cret", ""):
            with self.subTest(token=token):
                with mock.patch.dict(os.environ, {"GITHUB_TOKEN": token}), \
                        mock.patch("urllib.request.urlopen", fake_urlopen):
                    self.assertEqual(site_build.fetch_releases(REPO), [])
                request = captured["request"]
                self.assertEqual(request.full_url,
                                 f"https://api.github.com/repos/{REPO}/releases?per_page=1")
                self.assertEqual(captured["timeout"], 30)
                headers = {k.lower(): v for k, v in request.header_items()}
                self.assertEqual(headers["accept"], "application/vnd.github+json")
                self.assertEqual(headers["user-agent"], "virtual-serial-studio-site-build")
                if token:
                    self.assertEqual(headers["authorization"], "Bearer s3cret")
                else:
                    self.assertNotIn("authorization", headers)


if __name__ == "__main__":
    unittest.main()
