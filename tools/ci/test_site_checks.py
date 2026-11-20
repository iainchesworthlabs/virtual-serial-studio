#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Self-test of the documentation site's checks.

Covers tools/ci/check_site_kit.py, check_site_address.py, check_site_layout.py, check_site_pages.py (with its DRAFT check) and
check_release_data.py, and the hooks that need no MkDocs: tools/docs/site_layout_hook.py, site_version_hook.py and
site_address_hook.py. Each check is shown to pass on a good tree and to fail, with a reason, on a bad one. It needs neither MkDocs
nor a built site (the layout check is given a small site made here), the palette tests are skipped without Node, and the tests of the
update feed are skipped without OpenSSL 3. The release data hook and the refresh have their own tests, in test_site_release.py. Run
this one with `python -I tools/ci/test_site_checks.py`, or through `unittest discover -s tools/ci -p "test_*.py"`.
"""

import contextlib
import importlib.util
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "docs"))

import check_release_data  # noqa: E402
import check_site_address  # noqa: E402
import check_site_kit  # noqa: E402
import check_site_layout  # noqa: E402
import check_site_pages  # noqa: E402
import plugin_sign  # noqa: E402
import site_address_hook  # noqa: E402
import site_layout_hook  # noqa: E402
import site_release_hook  # noqa: E402
import site_version_hook  # noqa: E402
import update_feed  # noqa: E402

KIT = check_site_kit.KIT
SITE = "https://iainchesworthlabs.com/virtual-serial-studio/"


def copy_kit(root: Path) -> None:
    shutil.copytree(REPO / KIT, root / KIT)
    (root / "docs").mkdir(exist_ok=True)
    shutil.copyfile(REPO / "docs" / "theme.css", root / "docs" / "theme.css")


class Kit(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        copy_kit(self.root)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_the_committed_copy_is_good(self) -> None:
        self.assertEqual(check_site_kit.check_sums(REPO), [])

    def test_an_edited_file_is_found(self) -> None:
        css = self.root / KIT / "icl.css"
        css.write_text(css.read_text(encoding="utf-8") + "\nbody { color: red; }\n", encoding="utf-8")
        problems = check_site_kit.check_sums(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("icl.css", problems[0])
        self.assertIn("edited", problems[0])

    def test_line_endings_are_not_compared(self) -> None:
        css = self.root / KIT / "icl.css"
        css.write_bytes(css.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        self.assertEqual(check_site_kit.check_sums(self.root), [])

    def test_a_missing_and_a_surplus_file_are_found(self) -> None:
        (self.root / KIT / "kit" / "theme.css").unlink()
        (self.root / KIT / "kit" / "extra.css").write_text("a {}\n", encoding="utf-8")
        text = "\n".join(check_site_kit.check_sums(self.root))
        self.assertIn("kit/theme.css is listed in SHA256SUMS.txt and is missing", text)
        self.assertIn("kit/extra.css is not in the kit's SHA256SUMS.txt", text)

    @unittest.skipUnless(shutil.which("node"), "the company's palette checker needs Node")
    def test_the_palette_passes_and_a_bad_one_fails(self) -> None:
        self.assertEqual(check_site_kit.check_palette(self.root), [])
        theme = self.root / "docs" / "theme.css"
        lines = theme.read_text(encoding="utf-8").splitlines()
        # The link colour as pale as the page: it fails the contrast rule for text.
        bad = [("  --app-link: light-dark(#9db4cc, #8ab4ff);" if "--app-link:" in line else line) for line in lines]
        theme.write_text("\n".join(bad) + "\n", encoding="utf-8")
        problems = check_site_kit.check_palette(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("R3", problems[0])

    @unittest.skipUnless(shutil.which("node"), "the company's palette checker needs Node")
    def test_another_slug_or_property_fails(self) -> None:
        theme = self.root / "docs" / "theme.css"
        good = theme.read_text(encoding="utf-8")
        theme.write_text(good.replace('[data-app="virtual-serial-studio"]', '[data-app="field-test"]'), encoding="utf-8")
        self.assertIn("does not match folder", "".join(check_site_kit.check_palette(self.root)))
        theme.write_text(good.replace("}", "  --app-extra: light-dark(#000000, #ffffff);\n}"), encoding="utf-8")
        self.assertIn("not allowed", "".join(check_site_kit.check_palette(self.root)))


class Manifest(unittest.TestCase):
    """docs/icl-app.json against the rules of the kit's README ("Show up in the product lists")."""

    FIELDS = {"version", "name", "family", "summary", "tags", "status", "icon", "glyph", "order"}

    def setUp(self) -> None:
        self.manifest = json.loads((REPO / "docs" / "icl-app.json").read_text(encoding="utf-8"))

    def test_the_fields_are_the_fields_of_the_kit(self) -> None:
        self.assertLessEqual(set(self.manifest), self.FIELDS)
        self.assertEqual(self.manifest["version"], 1)

    def test_the_words_fit_the_limits(self) -> None:
        self.assertTrue(0 < len(self.manifest["name"]) <= 40)
        self.assertTrue(0 < len(self.manifest["summary"]) <= 120)
        self.assertLessEqual(len(self.manifest["tags"]), 4)
        for tag in self.manifest["tags"]:
            self.assertTrue(0 < len(tag) <= 24)

    def test_the_family_the_status_and_the_order(self) -> None:
        self.assertIn(self.manifest["family"], {"audio", "games", "test-equipment", "pool"})
        self.assertIn(self.manifest["status"], {"available", "coming-soon"})
        self.assertTrue(0 <= self.manifest["order"] <= 999)

    def test_the_icon_is_the_file_the_site_publishes(self) -> None:
        self.assertEqual(self.manifest["icon"], "icon.svg")


MKDOCS = f"site_name: x\nsite_url: {SITE}\nrepo_url: https://github.com/example/repo\n"


class Address(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "docs").mkdir()
        (self.root / "mkdocs.yml").write_text(MKDOCS, encoding="utf-8")
        (self.root / "docs" / "a.md").write_text("A page. Its address is {{ site_url }}.\n", encoding="utf-8")
        (self.root / "README.md").write_text(f"The documentation is at {SITE}guide/.\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def put(self, name: str, text: str) -> None:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_the_repository_is_good(self) -> None:
        self.assertEqual(check_site_address.check(REPO), [])

    def test_a_good_tree_passes(self) -> None:
        self.assertEqual(check_site_address.check(self.root), [])

    def test_another_address_is_refused(self) -> None:
        (self.root / "mkdocs.yml").write_text(MKDOCS.replace("/virtual-serial-studio/", "/studio/"), encoding="utf-8")
        self.assertIn("the address of this site is", "".join(check_site_address.check(self.root)))

    def test_a_page_that_writes_the_host_or_the_old_address_is_refused(self) -> None:
        self.put("docs/a.md", f"See {SITE}guide/ and https://iainchesworthlabs.github.io/x/.\n")
        text = "\n".join(check_site_address.check(self.root))
        self.assertIn("writes the site's host out", text)
        self.assertIn("names the old address", text)

    def test_ci_md_and_the_plans_may_name_the_hosts(self) -> None:
        self.put("docs/ci.md", f"The site is at {SITE}; it was at iainchesworthlabs.github.io; the program asks virtual-serial-studio.com.\n")
        self.put("docs/plan/site.md", f"The site is at {SITE} and virtual-serial-studio.com forwards to it.\n")
        self.assertEqual(check_site_address.check(self.root), [])

    def test_the_readme_may_not_name_the_old_address_or_another_path(self) -> None:
        self.put("README.md", "Old: https://iainchesworthlabs.github.io/virtual-serial-studio/. Other: https://iainchesworthlabs.com/field-test/.\n")
        text = "\n".join(check_site_address.check(self.root))
        self.assertIn("README.md: names the old address", text)
        self.assertIn("is not under", text)

    def test_the_compiled_in_address_is_allowed_where_the_program_has_it(self) -> None:
        text = "https://virtual-serial-studio.com/updates/v1/feed.json\n"
        for name in ("apps/vss_gui/src/updates/update_types.hpp", "apps/vss_gui/src/support/app_identity.cpp",
                     "apps/vss_gui/src/support/tests/test_app_identity.cpp", "apps/vss_gui/translations/vss_gui_fr.ts",
                     "docs/updates.md", "docs/user-guide/updates.md", "docs/support.md", "docs/press.md", "changelog.d/x.added.md"):
            self.put(name, text)
        self.assertEqual(check_site_address.check(self.root), [])

    def test_the_compiled_in_address_is_refused_in_a_page_and_in_a_tool(self) -> None:
        self.put("docs/b.md", "Ask https://virtual-serial-studio.com/docs/.\n")
        self.put("tools/other.py", 'URL = "https://virtual-serial-studio.com/"\n')
        text = "\n".join(check_site_address.check(self.root))
        self.assertIn("docs/b.md: writes the compiled-in address", text)
        self.assertIn("tools/other.py: writes the compiled-in address", text)

    def test_files_of_other_kinds_and_build_output_are_not_read(self) -> None:
        self.put("apps/build/x.cpp", "virtual-serial-studio.com\n")  # a build folder under apps/
        self.put("apps/thing.bin", "virtual-serial-studio.com\n")
        self.assertEqual(check_site_address.check(self.root), [])


MKDOCS_LAYOUT = (MKDOCS + "exclude_docs: |\n  /ci.md\n  /plan/\nnav:\n  - Guide:\n      - Upload: upload.md\n")

LAYOUT = {
    "pages": [
        {"src": "index.md", "new": ""},
        {"src": "upload.md", "new": "guide/upload/"},
        {"src": "ci.md", "new": None},
    ]
}


def page(canonical: str) -> str:
    return f'<!doctype html><html lang="en-AU"><head><title>t</title><link rel="canonical" href="{canonical}"></head><body><h1>t</h1></body></html>'


class Layout(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "repo"
        self.site = Path(self.tmp.name) / "site"
        (self.root / "docs" / "plan").mkdir(parents=True)
        (self.root / "tools" / "docs").mkdir(parents=True)
        (self.root / "mkdocs.yml").write_text(MKDOCS_LAYOUT, encoding="utf-8")
        for name in ("index.md", "upload.md", "ci.md", "plan/site.md"):
            (self.root / "docs" / name).write_text("# t\n", encoding="utf-8")
        self.write_layout(LAYOUT)
        (self.site / "guide" / "upload").mkdir(parents=True)
        (self.site / "assets").mkdir()
        (self.site / "index.html").write_text(page(SITE), encoding="utf-8")
        (self.site / "guide" / "upload" / "index.html").write_text(page(SITE + "guide/upload/"), encoding="utf-8")
        (self.site / "assets" / "index.html").write_text("not a page of docs/", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write_layout(self, layout: dict) -> None:
        (self.root / "tools" / "docs" / "site_layout.json").write_text(json.dumps(layout), encoding="utf-8")

    def problems(self) -> str:
        return "\n".join(check_site_layout.check(self.site, self.root))

    def test_the_table_of_the_repository_is_complete(self) -> None:
        self.assertEqual(check_site_layout.table_problems(REPO), [])

    def test_a_good_site_passes(self) -> None:
        self.assertEqual(self.problems(), "")

    def test_a_lost_page_is_found(self) -> None:
        (self.site / "guide" / "upload" / "index.html").unlink()
        self.assertIn("guide/upload/: is not in the built site", self.problems())

    def test_a_wrong_canonical_address_is_found(self) -> None:
        (self.site / "guide" / "upload" / "index.html").write_text(page(SITE + "upload/"), encoding="utf-8")
        self.assertIn("its canonical address is", self.problems())

    def test_a_page_published_that_should_be_on_github_only_is_found(self) -> None:
        (self.site / "ci").mkdir()
        (self.site / "ci" / "index.html").write_text(page(SITE + "ci/"), encoding="utf-8")
        self.assertIn("ci/: a page of the built site is at an address that site_layout.json does not give", self.problems())

    def test_a_page_left_at_the_address_of_its_file_name_is_found(self) -> None:
        (self.site / "upload").mkdir()
        (self.site / "upload" / "index.html").write_text(page(SITE + "upload/"), encoding="utf-8")
        self.assertIn("upload/: a page of the built site is at an address", self.problems())

    def test_a_page_left_under_user_guide_is_found(self) -> None:
        (self.site / "user-guide").mkdir()
        self.assertIn("still under user-guide/", self.problems())

    def test_a_missing_source_file_is_found(self) -> None:
        (self.root / "docs" / "ci.md").unlink()
        self.assertIn("docs/ci.md is not in docs/", self.problems())

    def test_a_page_that_the_table_does_not_name_is_found(self) -> None:
        (self.root / "docs" / "new.md").write_text("# t\n", encoding="utf-8")
        self.assertIn("docs/new.md: is not in site_layout.json", self.problems())

    def test_a_page_of_an_excluded_folder_needs_no_entry(self) -> None:
        (self.root / "docs" / "plan" / "other.md").write_text("# t\n", encoding="utf-8")
        self.assertEqual(self.problems(), "")

    def test_a_page_of_the_navigation_needs_an_address(self) -> None:
        self.write_layout({"pages": [LAYOUT["pages"][0], {"src": "upload.md", "new": None}, LAYOUT["pages"][2]]})
        text = self.problems()
        self.assertIn("the navigation names docs/upload.md, which has no address", text)
        self.assertIn("has no address (GitHub only) but exclude_docs of mkdocs.yml does not name it", text)

    def test_an_address_for_an_excluded_page_and_a_shared_address_are_found(self) -> None:
        self.write_layout({"pages": LAYOUT["pages"][:2] + [{"src": "ci.md", "new": "guide/upload/"}]})
        text = self.problems()
        self.assertIn("has the address 'guide/upload/' but exclude_docs of mkdocs.yml names it", text)
        self.assertIn("guide/upload/ is the address of docs/upload.md and of docs/ci.md", text)


class Fake:
    """Just enough of MkDocs' File and Files for the layout hook."""

    class Inclusion:
        def __init__(self, excluded: bool) -> None:
            self.excluded = excluded

        def is_excluded(self) -> bool:
            return self.excluded

    def __init__(self, src: str, dest: str, excluded: bool = False) -> None:
        self.src_uri = src
        self.dest_uri = dest
        self.inclusion = Fake.Inclusion(excluded)


class FakeFiles:
    def __init__(self, *files: Fake) -> None:
        self.by_src = {f.src_uri: f for f in files}

    def get_file_from_path(self, path: str):  # noqa: ANN201
        return self.by_src.get(path)

    def __iter__(self):  # noqa: ANN204
        return iter(self.by_src.values())


class LayoutHook(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.original = site_layout_hook.LAYOUT
        site_layout_hook.LAYOUT = Path(self.tmp.name) / "site_layout.json"
        site_layout_hook.LAYOUT.write_text(json.dumps(LAYOUT), encoding="utf-8")

    def tearDown(self) -> None:
        site_layout_hook.LAYOUT = self.original
        self.tmp.cleanup()

    def test_a_page_gets_its_new_address(self) -> None:
        upload = Fake("upload.md", "upload/index.html")
        files = FakeFiles(Fake("index.md", "index.html"), upload, Fake("ci.md", "ci/index.html", excluded=True))
        site_layout_hook.on_files(files, {})
        self.assertEqual(upload.dest_uri, "guide/upload/index.html")

    def test_a_file_that_is_not_there_stops_the_build(self) -> None:
        files = FakeFiles(Fake("index.md", "index.html"), Fake("ci.md", "ci/index.html", excluded=True))
        with self.assertRaises(ValueError) as caught:
            site_layout_hook.on_files(files, {})
        self.assertIn("upload.md is not in docs/", str(caught.exception))

    def test_a_page_that_the_table_does_not_name_keeps_its_address(self) -> None:
        other = Fake("other.md", "other/index.html")
        files = FakeFiles(Fake("index.md", "index.html"), Fake("upload.md", "upload/index.html"), Fake("ci.md", "ci/index.html", excluded=True), other)
        site_layout_hook.on_files(files, {})
        self.assertEqual(other.dest_uri, "other/index.html")

    def test_excluding_a_page_that_has_an_address_and_publishing_one_that_has_none_stop_the_build(self) -> None:
        files = FakeFiles(Fake("index.md", "index.html"), Fake("upload.md", "x", excluded=True), Fake("ci.md", "ci/index.html"))
        with self.assertRaises(ValueError) as caught:
            site_layout_hook.on_files(files, {})
        self.assertIn("excludes it", str(caught.exception))
        self.assertIn("does not exclude it", str(caught.exception))

    def test_the_table_of_the_repository_names_each_page_once(self) -> None:
        sources = [entry["src"] for entry in json.loads((REPO / "tools" / "docs" / "site_layout.json").read_text(encoding="utf-8"))["pages"]]
        self.assertEqual(len(sources), len(set(sources)))
        self.assertIn("user-guide/index.md", sources)
        self.assertNotIn("user-guide.md", sources)


def front(title: str, body: str = "<h1>t</h1>", head: str = "", description: str | None = None, lang: str = "en-AU") -> str:
    description = f"About {title}." if description is None else description
    return (f'<!doctype html><html lang="{lang}" data-app="virtual-serial-studio"><head><meta charset="utf-8"><title>{title}</title>'
            f'<meta name="description" content="{description}"><link rel="canonical" href="{SITE}{title}/">{head}'
            f'</head><body><main id="main">{body}</main></body></html>')


def fixed_site(site: Path) -> None:
    """The files that check_site_pages.FIXED asks for, as plain front pages."""
    for name in check_site_pages.FIXED:
        path = site / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if name.endswith(".html"):
            address = name.replace("index.html", "")
            body = "<h1>t</h1><p>DRAFT</p>" if address in check_site_pages.DRAFT_PAGES else "<h1>t</h1>"
            path.write_text(front(name.replace("/index.html", "") or "home", body=body), encoding="utf-8")
        elif name.endswith(".css"):
            path.write_text("a {}\n", encoding="utf-8")
        elif name.endswith(".json"):
            path.write_text("{}\n", encoding="utf-8")
        else:
            path.write_text("<svg/>\n", encoding="utf-8")


class Pages(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.site = Path(self.tmp.name)
        fixed_site(self.site)
        (self.site / "faq").mkdir()
        (self.site / "press").mkdir()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def problems(self) -> str:
        return "\n".join(check_site_pages.check_site(self.site, REPO))

    def put(self, name: str, html: str) -> None:
        (self.site / name).write_text(html, encoding="utf-8")

    def test_a_good_site_passes(self) -> None:
        self.assertEqual(self.problems(), "")

    def test_the_required_files_are_the_ones_this_site_has(self) -> None:
        self.assertEqual(set(check_site_pages.FIXED), {"index.html", "guide/index.html", "developers/index.html", "downloads/index.html",
                                                       "privacy/index.html", "support/index.html", "theme.css", "icon.svg", "icl-app.json"})

    def test_the_company_contract_pages_are_required(self) -> None:
        for name in ("privacy/index.html", "support/index.html"):
            (self.site / name).unlink()
            self.assertIn(f"{name}: is not in the built site", self.problems())
            self.put(name, front(name.replace("/index.html", "")))

    def test_a_missing_required_page_is_found(self) -> None:
        (self.site / "guide" / "index.html").unlink()
        self.assertIn("guide/index.html: is not in the built site", self.problems())

    def test_a_script_a_style_and_an_outside_load_on_a_front_page_are_found(self) -> None:
        self.put("faq/index.html", front("faq", head='<script src="x.js"></script><style>a{}</style><link rel="stylesheet" href="https://example.com/x.css">'))
        text = self.problems()
        self.assertIn("has a <script>", text)
        self.assertIn("has <style> or a style= attribute", text)
        self.assertIn("not a link to a host this site may name", text)

    def test_json_ld_is_allowed_and_inline_style_is_not(self) -> None:
        self.put("faq/index.html", front("faq", head='<script type="application/ld+json">{}</script>'))
        self.assertEqual(self.problems(), "")
        self.put("faq/index.html", front("faq", body='<h1 style="color:red">t</h1>'))
        self.assertIn("style=", self.problems())

    def test_the_company_shell_may_link_to_this_site_and_the_page_may_not(self) -> None:
        shell = '<header class="icl-header icl-header--next"><a href="/virtual-serial-studio/">Virtual Serial Studio</a></header><main id="main">'
        self.put("faq/index.html", front("faq").replace('<main id="main">', shell))
        self.assertEqual(self.problems(), "")
        self.put("faq/index.html", front("faq", body='<h1>t</h1><a href="/virtual-serial-studio/guide/">guide</a>'))
        self.assertIn("/virtual-serial-studio/guide/: write a relative link", self.problems())

    def test_the_company_footer_is_exempt_too(self) -> None:
        footer = '</main><footer class="icl-footer"><a href="/virtual-serial-studio/">Virtual Serial Studio</a></footer>'
        self.put("faq/index.html", front("faq").replace("</main>", footer))
        self.assertEqual(self.problems(), "")

    def test_a_mailto_link_is_found(self) -> None:
        self.put("faq/index.html", front("faq", body='<h1>t</h1><a href="mailto:someone@example.com">mail</a>'))
        self.assertIn("no mailto: link", self.problems())

    def test_a_link_to_a_missing_file_or_anchor_is_found(self) -> None:
        self.put("faq/index.html", front("faq", body='<h1>t</h1><a href="../nothing/">x</a><a href="../guide/#missing">y</a>'))
        text = self.problems()
        self.assertIn("../nothing/: no such file in the built site", text)
        self.assertIn("no anchor #missing", text)

    def test_a_link_that_works_is_not_flagged_and_a_path_of_this_site_is(self) -> None:
        self.put("faq/index.html", front("faq", body='<h1>t</h1><a href="../guide/">x</a><a href="/support/">company</a>'))
        self.assertEqual(self.problems(), "")
        self.put("faq/index.html", front("faq", body='<h1>t</h1><a href="/virtual-serial-studio/guide/">x</a>'))
        self.assertIn("write a relative link", self.problems())

    def test_a_picture_needs_alt_and_a_front_page_picture_needs_a_size(self) -> None:
        self.put("faq/index.html", front("faq", body='<h1>t</h1><img src="../icon.svg" width="1" height="1">'))
        self.assertIn("has no alt attribute", self.problems())
        self.put("faq/index.html", front("faq", body='<h1>t</h1><img src="../icon.svg" alt="x">'))
        self.assertIn("has no width and height", self.problems())

    def test_headings_may_not_skip_a_level_and_start_with_h1(self) -> None:
        self.put("faq/index.html", front("faq", body="<h1>t</h1><h2>a</h2><h4>b</h4>"))
        self.assertIn("goes from h2 to h4", self.problems())
        self.assertEqual(check_site_pages.check_headings("x", [2, 3]), ["x: the first heading is an h2, not the h1"])

    def test_two_front_pages_may_not_share_a_title_or_a_description(self) -> None:
        self.put("faq/index.html", front("same", description="One description."))
        self.put("press/index.html", front("same", description="One description."))
        text = self.problems()
        self.assertIn("has the same title as", text)
        self.assertIn("has the same description as", text)

    def test_a_front_page_needs_its_language_and_one_h1(self) -> None:
        self.put("faq/index.html", front("faq", body="<h1>a</h1><h1>b</h1>", lang="en"))
        text = self.problems()
        self.assertIn('needs lang="en-AU"', text)
        self.assertIn("has 2 <h1>", text)

    def test_a_stylesheet_that_loads_from_another_host_is_found(self) -> None:
        (self.site / "theme.css").write_text("@import url(https://example.com/x.css);\n", encoding="utf-8")
        self.assertIn("loads something with @import or an address", self.problems())

    def test_a_guide_page_may_have_a_form_of_its_own(self) -> None:
        material = (f'<!doctype html><html lang="en"><head><title>g</title><link rel="canonical" href="{SITE}g/"></head>'
                    '<body><div class="md-container"><form class="md-search__form" action="x"><input name="q"></form>'
                    '<article class="md-typeset"><h1>g</h1></article></div></body></html>')
        self.put("guide/index.html", material)
        self.assertEqual(self.problems(), "")


# A contact form that the check accepts. The form itself arrives with the front pages (docs/plan/site.md); the rules are held
# here, so that the page that adds it cannot drift from the endpoint's contract.
CONTACT_FORM = """<form class="vss-form" method="post" action="/api/contact">
<input type="hidden" name="source" value="virtual-serial-studio">
<div><label for="cf-name">Name</label><input id="cf-name" name="name" type="text" autocomplete="name" maxlength="100" required></div>
<div><label for="cf-organisation">Organisation</label><input id="cf-organisation" name="organisation" type="text" autocomplete="organization" maxlength="150"></div>
<div><label for="cf-email">Email</label><input id="cf-email" name="email" type="email" autocomplete="email" maxlength="254" required></div>
<div><label for="cf-topic">Topic</label><select id="cf-topic" name="topic" required>
<option value="">Choose a topic</option>
<option value="pro">Pro</option>
<option value="enterprise">Enterprise</option>
<option value="trial">A trial</option>
<option value="plugins">Plugins</option>
<option value="other">Something else</option>
</select></div>
<div><label for="cf-message">Message</label><textarea id="cf-message" name="message" maxlength="4000" required></textarea></div>
<div aria-hidden="true"><label for="cf-ref">Leave this empty</label><input id="cf-ref" name="ref_code" type="text" tabindex="-1" autocomplete="off"></div>
<div><input id="cf-consent" name="consent" type="checkbox" value="yes" required><label for="cf-consent">I agree to what the form keeps (<a href="/privacy/#contact-form">privacy</a>).</label></div>
<button class="icl-button" type="submit">Send</button>
</form>"""


class ContactForm(unittest.TestCase):
    """The form rules of check_site_pages, with the form above and the changes the check has to notice."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.site = Path(self.tmp.name)
        fixed_site(self.site)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def problems(self, form: str = CONTACT_FORM) -> str:
        (self.site / "support" / "index.html").write_text(front("support", body=f"<h1>t</h1>{form}"), encoding="utf-8")
        return "\n".join(check_site_pages.check_site(self.site, REPO))

    def changed(self, old: str, new: str) -> str:
        self.assertIn(old, CONTACT_FORM, "the test expects this text in the form")
        return self.problems(CONTACT_FORM.replace(old, new, 1))

    def test_the_constants_are_this_products(self) -> None:
        self.assertEqual(check_site_pages.FORM_SOURCE, "virtual-serial-studio")
        self.assertEqual(check_site_pages.FORM_TOPICS, {"", "pro", "enterprise", "trial", "plugins", "other"})

    def test_the_form_passes(self) -> None:
        self.assertEqual(self.problems(), "")

    def test_it_posts_to_the_one_endpoint_and_nowhere_else(self) -> None:
        self.assertIn("it may post only to /api/contact", self.changed('action="/api/contact"', 'action="https://example.com/api/contact"'))
        self.assertIn("it may post only to /api/contact", self.changed('action="/api/contact"', 'action="/api/other"'))
        self.assertIn("it may post only to /api/contact", self.changed('action="/api/contact"', 'action="/virtual-serial-studio/api/contact"'))
        self.assertIn('method="post"', self.changed('method="post"', 'method="get"'))
        self.assertIn("enctype must be the default", self.changed('method="post"', 'method="post" enctype="multipart/form-data"'))

    def test_a_second_form_is_refused(self) -> None:
        self.assertIn("has 2 forms", self.problems(CONTACT_FORM + '<form action="/x"><input name="q"></form>'))

    def test_the_fields_are_the_fields_of_the_endpoint(self) -> None:
        self.assertIn("the form's fields are", self.changed('name="organisation"', 'name="company"'))
        self.assertIn("the form's fields are", self.problems(CONTACT_FORM.replace("</form>", '<input name="phone"></form>')))
        self.assertIn("names a field twice", self.problems(CONTACT_FORM.replace("</form>", '<input type="hidden" name="name" value="x"></form>')))
        self.assertIn('"source" must be a hidden field with the value "virtual-serial-studio"', self.changed('value="virtual-serial-studio"', 'value="other"'))

    def test_every_field_has_a_label_and_the_endpoints_limits(self) -> None:
        self.assertIn('the field "name" has no <label for>', self.changed('<label for="cf-name">', '<label for="somewhere-else">'))
        self.assertIn('the field "name" is required', self.changed('autocomplete="name" maxlength="100" required', 'autocomplete="name" maxlength="100"'))
        self.assertIn('the field "message" needs maxlength="4000"', self.changed('maxlength="4000"', 'maxlength="9000"'))
        self.assertIn('the field "email" needs type="email"', self.changed('id="cf-email" name="email" type="email"', 'id="cf-email" name="email" type="text"'))
        self.assertIn('the field "message" must be a <textarea>', self.changed('<textarea id="cf-message" name="message"', '<input id="cf-message" name="message" type="text"').replace("</textarea>", ""))

    def test_the_topics_are_the_topics_of_the_endpoint(self) -> None:
        self.assertIn('the field "topic" must be a <select>', self.changed('<option value="enterprise">', '<option value="enterprise-plus">'))
        self.assertIn('the field "topic" must be a <select>', self.changed('<option value="plugins">Plugins</option>\n', ""))

    def test_the_honeypot_must_be_hidden_and_out_of_the_tab_order(self) -> None:
        self.assertIn("the honeypot", self.changed(' aria-hidden="true"', ""))
        self.assertIn("the honeypot", self.changed('tabindex="-1"', ""))
        self.assertIn("the honeypot", self.changed('type="text" tabindex="-1" autocomplete="off"', 'type="text" tabindex="-1"'))
        self.assertIn("the honeypot", self.changed('tabindex="-1" autocomplete="off"', 'tabindex="-1" autocomplete="off" required'))

    def test_consent_and_the_privacy_link_and_a_submit_button(self) -> None:
        self.assertIn('the field "consent" must be a checkbox with the value "yes"', self.changed('type="checkbox" value="yes"', 'type="checkbox" value="on"'))
        self.assertIn("must link to /privacy/#contact-form", self.problems(CONTACT_FORM.replace("/privacy/#contact-form", "/privacy/")))
        self.assertIn("has no submit button", self.problems(CONTACT_FORM.replace('<button class="icl-button" type="submit">', '<button class="icl-button" type="button">')))

    def test_a_script_on_the_form_page_is_still_refused(self) -> None:
        self.assertIn("has a <script>", self.problems(CONTACT_FORM + "<script>x()</script>"))


class Drafts(unittest.TestCase):
    """check_site_drafts: the licence, privacy and pricing pages say DRAFT in their main content while they are built."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.site = Path(self.tmp.name)
        fixed_site(self.site)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def put(self, address: str, html: str) -> None:
        (self.site / address).mkdir(parents=True, exist_ok=True)
        (self.site / address / "index.html").write_text(html, encoding="utf-8")

    def test_the_pages_are_the_three_of_the_plan(self) -> None:
        self.assertEqual(check_site_pages.DRAFT_PAGES, ["licensing/", "privacy/", "pricing/"])

    def test_no_page_built_is_not_an_error_and_is_named(self) -> None:
        self.assertEqual(check_site_pages.check_site_drafts(self.site), [])
        # privacy/ is a required page (FIXED), so fixed_site builds it, with the word.
        self.assertEqual(check_site_pages.absent_drafts(self.site), ["licensing/", "pricing/"])

    def test_a_page_with_the_word_passes(self) -> None:
        for address in check_site_pages.DRAFT_PAGES:
            self.put(address, front(address.strip("/"), body="<h1>t</h1><p><strong>DRAFT</strong> for a solicitor.</p>"))
        self.assertEqual(check_site_pages.check_site_drafts(self.site), [])
        self.assertEqual(check_site_pages.absent_drafts(self.site), [])

    def test_a_page_without_the_word_is_found(self) -> None:
        self.put("privacy/", front("privacy", body="<h1>Privacy</h1><p>Final text.</p>"))
        self.put("pricing/", front("pricing", body="<h1>Pricing</h1><p>DRAFT prices.</p>"))
        problems = check_site_pages.check_site_drafts(self.site)
        self.assertEqual(len(problems), 1)
        self.assertIn("privacy/index.html: the main content does not contain the word DRAFT", problems[0])

    def test_only_the_main_content_counts(self) -> None:
        in_the_shell = front("licensing", body="<h1>Licensing</h1>").replace("<main", "<header>DRAFT</header><main")
        self.put("licensing/", in_the_shell)
        self.assertEqual(len(check_site_pages.check_site_drafts(self.site)), 1)
        in_a_script = front("licensing", body='<h1>Licensing</h1><script type="application/ld+json">"DRAFT"</script>')
        self.put("licensing/", in_a_script)
        self.assertEqual(len(check_site_pages.check_site_drafts(self.site)), 1)

    def test_it_is_the_word_and_not_part_of_one(self) -> None:
        self.put("pricing/", front("pricing", body="<h1>Pricing</h1><p>DRAFTS and REDRAFT.</p>"))
        self.assertEqual(len(check_site_pages.check_site_drafts(self.site)), 1)
        self.put("pricing/", front("pricing", body="<h1>Pricing</h1><p>This is a DRAFT.</p>"))
        self.assertEqual(check_site_pages.check_site_drafts(self.site), [])

    def test_a_material_page_counts_its_article(self) -> None:
        material = (f'<!doctype html><html lang="en"><head><title>p</title><link rel="canonical" href="{SITE}privacy/"></head>'
                    '<body><div class="md-container"><article class="md-typeset"><h1>p</h1><p>DRAFT</p></article></div></body></html>')
        self.put("privacy/", material)
        self.assertEqual(check_site_pages.check_site_drafts(self.site), [])

    def test_the_whole_check_includes_it_and_the_command_names_what_is_absent(self) -> None:
        self.put("pricing/", front("pricing", body="<h1>Pricing</h1><p>Final.</p>"))
        self.assertIn("pricing/index.html: the main content does not contain the word DRAFT", "\n".join(check_site_pages.check_site(self.site, REPO)))
        self.put("pricing/", front("pricing", body="<h1>Pricing</h1><p>DRAFT</p>"))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(check_site_pages.main(["--site", str(self.site), "--root", str(REPO)]), 0)
        self.assertIn("Site pages: ok", out.getvalue())
        self.assertIn("Site drafts: licensing/ not in the built site yet", out.getvalue())


def openssl_problem() -> str:
    """Why OpenSSL 3 cannot be used here, or an empty text."""
    try:
        plugin_sign.find_openssl()
    except plugin_sign.Refusal as refusal:
        return refusal.message
    return ""


OPENSSL_PROBLEM = openssl_problem()
REAL_KEYS_FILE = REPO / "libs" / "vss_update" / "src" / "trusted_keys_release.cpp"
RELEASE_FEED = "updates/v1/feed.json"
RELEASES = "https://github.com/example/site/releases/"


def keys_file_text(public_key: bytes) -> str:
    """A file in the form of libs/vss_update/src/trusted_keys_release.cpp with one key, the public half of a throwaway key."""
    items = ", ".join("0x%02x" % b for b in public_key)
    return (
        # In two parts, so that the licence check does not take it for this file's own tag.
        "// SPDX-" "License-Identifier: GPL-3.0-or-later\n"
        "namespace vss::update\n{\n"
        "namespace\n{\n"
        "constexpr public_key upd_2026_a{%s};\n"
        "trusted_key make(std::string_view key_id, std::string_view purpose,\n"
        "                 const public_key& key)\n"
        "{\n    trusted_key entry;\n    entry.key = key;\n    return entry;\n}\n"
        "} // namespace\n"
        "std::span<const trusted_key> builtin_feed_keys()\n{\n"
        "    static const std::array<trusted_key, 1> keys{\n"
        '        make("upd-2026-a", feed_purpose, upd_2026_a)};\n'
        "    return keys;\n}\n}\n"
    ) % items


class ReleaseData(unittest.TestCase):
    """check_release_data.check_addresses: release.json holds only addresses of this repository's releases."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.folder = self.root / site_release_hook.RELEASE_DIR
        self.folder.mkdir(parents=True)
        (self.root / "mkdocs.yml").write_text("site_name: x\nrepo_url: https://github.com/example/site\n", encoding="utf-8")
        shutil.copyfile(REPO / site_release_hook.RELEASE_DIR / site_release_hook.RULES_NAME, self.folder / site_release_hook.RULES_NAME)
        self.write(self.record())

    def tearDown(self) -> None:
        self.tmp.cleanup()

    @staticmethod
    def record(**changes: object) -> dict:
        data = {"tag": "v0.2.0", "prerelease": False, "published_at": "2026-12-01T10:00:00Z", "html_url": RELEASES + "tag/v0.2.0",
                "assets": [{"name": "a.deb", "size": 1, "url": RELEASES + "download/v0.2.0/a.deb"}]}
        data.update(changes)
        return data

    def write(self, data: dict) -> None:
        (self.folder / site_release_hook.RELEASE_NAME).write_text(json.dumps(data), encoding="utf-8")

    def test_the_data_of_the_repository_is_good(self) -> None:
        self.assertEqual(check_release_data.check_addresses(REPO), [])
        self.assertEqual(check_release_data.check(REPO), [])

    def test_a_good_record_passes(self) -> None:
        self.assertEqual(check_release_data.check_addresses(self.root), [])

    def test_an_address_of_another_repository_or_host_is_found(self) -> None:
        for label, bad in (("another repository", "https://github.com/other/repo/releases/download/v1/a.deb"),
                           ("another host", "https://example.com/example/site/releases/download/v1/a.deb"),
                           ("a look-alike", "https://github.com/example/site-evil/releases/download/v1/a.deb"),
                           ("http", "http://github.com/example/site/releases/download/v1/a.deb"),
                           ("the repository but not its releases", "https://github.com/example/site/archive/v1.zip")):
            with self.subTest(label):
                data = self.record()
                data["assets"][0]["url"] = bad
                self.write(data)
                text = "\n".join(check_release_data.check_addresses(self.root))
                self.assertIn("release.json.assets[0].url", text)
                self.assertIn(f"does not begin with {RELEASES}", text)

    def test_the_release_page_address_is_held_too(self) -> None:
        self.write(self.record(html_url="https://github.com/example/site/issues/1"))
        self.assertIn("release.json.html_url", "\n".join(check_release_data.check_addresses(self.root)))

    def test_every_address_is_reported_and_a_text_with_no_address_is_not(self) -> None:
        data = self.record(note="no address here", more=["https://evil.test/x", "https://evil.test/y"])
        self.write(data)
        problems = check_release_data.check_addresses(self.root)
        self.assertEqual(len(problems), 2)

    def test_a_record_the_site_build_would_refuse_is_reported_with_the_reason(self) -> None:
        self.write(self.record(prerelease="yes"))
        self.assertIn("'prerelease' must be true or false", "\n".join(check_release_data.check_addresses(self.root)))

    def test_a_missing_or_unreadable_file_is_reported(self) -> None:
        (self.folder / site_release_hook.RELEASE_NAME).write_text("{not json", encoding="utf-8")
        self.assertIn("cannot be read", "\n".join(check_release_data.check_addresses(self.root)))
        (self.folder / site_release_hook.RELEASE_NAME).unlink()
        self.assertIn("cannot be read", "\n".join(check_release_data.check_addresses(self.root)))


@unittest.skipIf(bool(OPENSSL_PROBLEM), "OpenSSL 3 is not on the PATH: " + OPENSSL_PROBLEM)
class ReleaseFeed(unittest.TestCase):
    """The update feed of the built site (updates/v1/feed.json) against the keys Studio trusts and against the stored copy. The keys are
    throwaway keys that OpenSSL makes for these tests."""

    @classmethod
    def setUpClass(cls) -> None:
        cls._keys = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls._keys.cleanup)
        openssl = plugin_sign.find_openssl()
        cls.keyfiles = []
        for name in ("a.pem", "b.pem"):
            path = Path(cls._keys.name) / name
            subprocess.run([openssl, "genpkey", "-algorithm", "ed25519", "-out", str(path)], check=True, capture_output=True)
            cls.keyfiles.append(path)
        cls.public = [plugin_sign.public_key_of(openssl, str(p)) for p in cls.keyfiles]

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "repo"
        self.site = Path(self.tmp.name) / "site"
        (self.root / site_release_hook.RELEASE_DIR).mkdir(parents=True)
        self.site.mkdir()
        self.source = self.root / site_release_hook.RELEASE_DIR / site_release_hook.FEED_NAME

    def signed_feed(self, key_index: int = 0, key_id: str = "upd-2026-a") -> bytes:
        payload = update_feed.build_payload(7, "2026-12-01T03:40:00Z", [], [])
        text = plugin_sign.sign_envelope(plugin_sign.find_openssl(), str(self.keyfiles[key_index]), "update-feed", key_id, payload.encode("utf-8"))
        return text.encode("ascii")

    def changed_feed(self) -> bytes:
        found = plugin_sign.parse_envelope(self.signed_feed())
        payload = found.payload.replace(b'"serial": 7', b'"serial": 8')
        self.assertNotEqual(payload, found.payload)
        return plugin_sign.envelope_text(found.purpose, found.key_id, payload, found.signature).encode("ascii")

    def keys_file(self, public_key: bytes | None = None) -> Path:
        path = Path(self.tmp.name) / "trusted_keys_release.cpp"
        path.write_text(keys_file_text(public_key or self.public[0]), encoding="utf-8")
        return path

    def publish(self, built: bytes | None, source: bytes | None = ...) -> list[str]:  # type: ignore[assignment]
        """Put a feed in the built site and, by default, the same bytes in the release data. `source=None` means no stored copy."""
        if source is ...:
            source = built
        shutil.rmtree(self.site / "updates", ignore_errors=True)
        self.source.unlink(missing_ok=True)
        if built is not None:
            target = self.site / RELEASE_FEED
            target.parent.mkdir(parents=True)
            target.write_bytes(built)
        if source is not None:
            self.source.write_bytes(source)
        return check_release_data.check_feed(self.root, self.site, self.keys_file())

    def test_no_feed_in_either_place_is_nothing_to_check(self) -> None:
        self.assertEqual(self.publish(None), [])
        # Whatever the keys: the file is not read when there is no feed.
        self.assertEqual(check_release_data.check_feed(self.root, self.site, REAL_KEYS_FILE), [])

    def test_a_feed_that_verifies_and_is_the_stored_one_passes(self) -> None:
        self.assertEqual(self.publish(self.signed_feed()), [])

    def test_a_feed_that_does_not_verify_is_found(self) -> None:
        for label, feed in (
            ("another key", self.signed_feed(key_index=1)),
            ("a changed payload", self.changed_feed()),
            ("a key id that is not trusted", self.signed_feed(key_id="upd-2026-b")),
            ("not an envelope", b"{}"),
            ("empty", b""),
            ("the wrong purpose", plugin_sign.sign_envelope(plugin_sign.find_openssl(), str(self.keyfiles[0]), "plugin", "upd-2026-a", b"{}").encode("ascii")),
        ):
            with self.subTest(label):
                problems = self.publish(feed)
                self.assertEqual(len(problems), 1)
                self.assertTrue(problems[0].startswith(RELEASE_FEED + ": "), problems[0])
                self.assertNotIn("\n", problems[0])

    def test_a_feed_when_the_build_trusts_no_key_is_found(self) -> None:
        self.publish(self.signed_feed())
        problems = check_release_data.check_feed(self.root, self.site, REAL_KEYS_FILE)
        self.assertEqual(len(problems), 1)
        self.assertIn("trusts no update feed key", problems[0])

    def test_a_keys_file_that_cannot_be_read_is_found(self) -> None:
        self.publish(self.signed_feed())
        bad = Path(self.tmp.name) / "bad.cpp"
        bad.write_text(keys_file_text(bytes(31)), encoding="utf-8")
        self.assertEqual(len(check_release_data.check_feed(self.root, self.site, bad)), 1)
        self.assertEqual(len(check_release_data.check_feed(self.root, self.site, Path(self.tmp.name) / "missing.cpp")), 1)

    def test_the_built_feed_must_be_the_stored_bytes(self) -> None:
        feed = self.signed_feed()
        problems = self.publish(feed, source=feed + b"\n")
        self.assertEqual(problems, [f"{RELEASE_FEED}: is not the same bytes as tools/docs/release/update-feed.json"])
        problems = self.publish(feed, source=self.changed_feed())
        self.assertEqual(len(problems), 1)
        self.assertIn("is not the same bytes", problems[0])

    def test_line_endings_are_bytes_too(self) -> None:
        feed = self.signed_feed()
        self.assertEqual(len(self.publish(feed, source=feed.replace(b"\n", b"\r\n"))), 1)

    def test_a_feed_in_one_place_and_not_the_other_is_found(self) -> None:
        feed = self.signed_feed()
        problems = self.publish(feed, source=None)
        self.assertEqual(problems, [f"{RELEASE_FEED}: the built site has an update feed, but tools/docs/release/update-feed.json does not exist"])
        problems = self.publish(None, source=feed)
        self.assertEqual(len(problems), 1)
        self.assertIn(f"the built site has no {RELEASE_FEED}", problems[0])

    def test_the_command_checks_the_feed_with_the_given_keys(self) -> None:
        (self.root / "mkdocs.yml").write_text("repo_url: https://github.com/example/site\n", encoding="utf-8")
        shutil.copyfile(REPO / site_release_hook.RELEASE_DIR / site_release_hook.RULES_NAME, self.source.with_name(site_release_hook.RULES_NAME))
        (self.source.with_name(site_release_hook.RELEASE_NAME)).write_text(json.dumps(ReleaseData.record()), encoding="utf-8")
        self.publish(self.signed_feed())
        for keys, status in ((self.keys_file(), 0), (REAL_KEYS_FILE, 1)):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = check_release_data.main(["--root", str(self.root), "--site", str(self.site), "--keys", str(keys)])
            self.assertEqual(code, status, out.getvalue())
        self.assertIn("::error::updates/v1/feed.json: ", out.getvalue())

    def test_the_command_says_when_there_is_no_feed_to_check(self) -> None:
        (self.root / "mkdocs.yml").write_text("repo_url: https://github.com/example/site\n", encoding="utf-8")
        shutil.copyfile(REPO / site_release_hook.RELEASE_DIR / site_release_hook.RULES_NAME, self.source.with_name(site_release_hook.RULES_NAME))
        (self.source.with_name(site_release_hook.RELEASE_NAME)).write_text(json.dumps(ReleaseData.record()), encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(check_release_data.main(["--root", str(self.root), "--site", str(self.site)]), 0)
        self.assertIn("the site has no updates/v1/feed.json, so there is no feed to check", out.getvalue())


class ReleaseFeedOfTheRepository(unittest.TestCase):
    def test_the_repository_has_no_feed_key_yet_and_a_feed_would_be_found_today(self) -> None:
        self.assertEqual(update_feed.read_feed_keys(str(REAL_KEYS_FILE)), [])
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        site = Path(tmp.name)
        (site / "updates" / "v1").mkdir(parents=True)
        (site / RELEASE_FEED).write_bytes(b"{}")
        problems = check_release_data.check_feed(REPO, site)
        self.assertTrue(any("trusts no update feed key" in problem for problem in problems), problems)

    def test_the_command_has_a_help_and_the_default_keys_are_the_repositorys(self) -> None:
        self.assertEqual(REPO / check_release_data.KEYS_FILE, REAL_KEYS_FILE)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(SystemExit) as caught:
            check_release_data.main(["--help"])
        self.assertEqual(caught.exception.code, 0)
        self.assertIn("--site", out.getvalue())

    def test_a_site_that_is_not_a_folder_is_a_usage_error(self) -> None:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(check_release_data.main(["--site", str(REPO / "no-such-folder")]), 2)


class Version(unittest.TestCase):
    def test_the_version_is_read_from_the_project_command(self) -> None:
        text = "cmake_minimum_required(VERSION 3.21)\nproject(\n  virtual_serial_studio\n  VERSION 1.22.3\n  LANGUAGES CXX)\n"
        self.assertEqual(site_version_hook.read_version(text), "1.22.3")

    def test_the_repository_has_one(self) -> None:
        text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(site_version_hook.read_version(text), r"^\d+\.\d+\.\d+$")

    def test_a_missing_version_stops_the_build(self) -> None:
        with self.assertRaises(ValueError):
            site_version_hook.read_version("project(other VERSION 1.0.0)\n")


class AddressHook(unittest.TestCase):
    def test_the_token_becomes_the_address_with_one_slash(self) -> None:
        self.assertEqual(site_address_hook.fill("See {{ site_url }}guide/.", SITE), f"See {SITE}guide/.")
        self.assertEqual(site_address_hook.fill("See {{ site_url }}guide/.", SITE.rstrip("/")), f"See {SITE}guide/.")


@unittest.skipUnless(importlib.util.find_spec("jinja2"), "the shell hook needs Jinja, which MkDocs brings")
class ShellHook(unittest.TestCase):
    def test_the_palette_name_goes_on_the_html_tag_once(self) -> None:
        import site_shell_hook  # noqa: PLC0415 (imported here because it needs Jinja)

        out = site_shell_hook.add_app('<!doctype html><html lang="en"><head></head></html>')
        self.assertIn('<html data-app="virtual-serial-studio" lang="en">', out)
        self.assertEqual(site_shell_hook.add_app(out), out)
        with self.assertRaises(ValueError):
            site_shell_hook.add_app("<p>no html tag</p>")


if __name__ == "__main__":
    unittest.main()
