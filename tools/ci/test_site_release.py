#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Self-test of the release data of the documentation site: tools/docs/site_release_hook.py and tools/docs/refresh_release.py.

The hook turns tools/docs/release/ into the Downloads table, the version and date markers and the copy of the update feed. The refresh
rewrites that folder from the GitHub API. Neither test needs MkDocs or a built site, and nothing here uses the network: the refresh is
given fake functions, and the code that opens a connection is replaced. Run it with `python -I tools/ci/test_site_release.py`, or through
`unittest discover -s tools/ci -p "test_*.py"`. The check of the data is in test_site_checks.py.
"""

import contextlib
import io
import json
import logging
import os
import re
import sys
import tempfile
import unittest
import unittest.mock
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
for folder in (HERE, REPO / "tools", REPO / "tools" / "docs"):
    sys.path.insert(0, str(folder))

import refresh_release  # noqa: E402
import site_release_hook as hook  # noqa: E402

REPO_NAME = "example/site"
PREFIX = f"https://github.com/{REPO_NAME}/releases/"

RULES = (
    hook.Rule(re.compile(r"-windows-x86_64-testsigned\.exe$"), "Windows", "Installer, with the test-signed driver"),
    hook.Rule(re.compile(r"-windows-x86_64\.exe$"), "Windows", "Installer"),
    hook.Rule(re.compile(r"\.deb$"), "Linux", "Debian package"),
    hook.Rule(re.compile(r"\.rpm$"), "Linux", "RPM package"),
)
RULES_JSON = {
    "rules": [
        {"match": r"-windows-x86_64-testsigned\.exe$", "platform": "Windows", "label": "Installer, with the test-signed driver"},
        {"match": r"\.deb$", "platform": "Linux", "label": "Debian package"},
    ]
}

# Bytes that no text reader would keep: a feed is copied, never read.
FEED = b'{"format":1,\r\n "not": "parsed or changed"}\r\n\x00\xff'


def asset(name: str, size: int = 1000, tag: str = "v0.1.0-rc.3") -> dict:
    return {"name": name, "size": size, "url": f"{PREFIX}download/{tag}/{name}"}


def release_json(**changes: object) -> dict:
    data = {
        "tag": "v0.1.0-rc.3",
        "prerelease": True,
        "published_at": "2026-11-12T11:18:55Z",
        "html_url": PREFIX + "tag/v0.1.0-rc.3",
        "assets": [],
    }
    data.update(changes)
    return data


def release_of(assets: list[dict], **changes: object) -> "hook.Release":
    data = release_json(assets=assets, **changes)
    return hook.Release(
        data["tag"], data["prerelease"], data["published_at"], data["html_url"],
        tuple(hook.Asset(a["name"], a["size"], a["url"]) for a in assets),
    )


class Folder(unittest.TestCase):
    """A repository with the release folder and a mkdocs.yml, in a temporary directory."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve() / "repo"
        self.folder = self.root / hook.RELEASE_DIR
        self.folder.mkdir(parents=True)
        (self.root / "docs").mkdir()
        (self.root / "mkdocs.yml").write_text(f"site_name: x\nrepo_url: https://github.com/{REPO_NAME}\n", encoding="utf-8")
        self.put("release.json", json.dumps(release_json(assets=[asset("a.deb", 10), asset("a.deb.asc", 1)])))
        self.put("download_rules.json", json.dumps(RULES_JSON))

    def put(self, name: str, content: str | bytes) -> Path:
        path = self.folder / name
        path.write_bytes(content.encode("utf-8") if isinstance(content, str) else content)
        return path

    def config(self, site_dir: str = "site") -> dict:
        return {"docs_dir": str(self.root / "docs"), "site_dir": str(self.root / site_dir), "extra": {}}


class Formatting(unittest.TestCase):
    def test_sizes(self) -> None:
        cases = {0: "0 kB", 1: "1 kB", 499: "1 kB", 500: "1 kB", 1_499: "1 kB", 1_500: "2 kB", 268_000: "268 kB",
                 999_999: "1000 kB", 1_000_000: "1.0 MB", 43_900_000: "43.9 MB", 32_253_845: "32.3 MB"}
        for size, text in cases.items():
            with self.subTest(size=size):
                self.assertEqual(hook.format_size(size), text)

    def test_dates(self) -> None:
        self.assertEqual(hook.format_date("2026-11-12T11:18:55Z"), "12 November 2026")
        self.assertEqual(hook.format_date("2026-01-01"), "1 January 2026")

    def test_bad_dates(self) -> None:
        for text in ("12 November 2026", "2026-13-01T00:00:00Z", "2026-00-10", ""):
            with self.subTest(text=text), self.assertRaises(hook.ReleaseDataError):
                hook.format_date(text)

    def test_the_kind_says_a_release_candidate_plainly(self) -> None:
        self.assertEqual(hook.kind_of(release_of([], tag="v0.1.0-rc.5", prerelease=True)), "release candidate")
        self.assertEqual(hook.kind_of(release_of([], tag="v0.2.0-beta.1", prerelease=True)), "pre-release")
        self.assertEqual(hook.kind_of(release_of([], tag="v0.1.0", prerelease=False)), "final release")

    def test_the_group_ids(self) -> None:
        self.assertEqual(hook.platform_id("Windows"), "dl-windows")
        self.assertEqual(hook.platform_id("Checksums and contents"), "dl-checksums-and-contents")
        self.assertEqual(hook.platform_id("???"), "dl-files")


class Loading(Folder):
    def test_a_good_record_loads(self) -> None:
        release = hook.load_release(self.folder / "release.json")
        self.assertEqual((release.tag, release.prerelease), ("v0.1.0-rc.3", True))
        self.assertEqual([a.name for a in release.assets], ["a.deb", "a.deb.asc"])
        self.assertEqual(len(hook.load_rules(self.folder / "download_rules.json")), 2)

    def refused(self, **changes: object) -> str:
        self.put("release.json", json.dumps(release_json(**changes)))
        with self.assertRaises(hook.ReleaseDataError) as caught:
            hook.load_release(self.folder / "release.json")
        return str(caught.exception)

    def test_what_is_refused(self) -> None:
        self.assertIn("'prerelease' must be true or false", self.refused(prerelease="yes"))
        self.assertIn("'tag' must be a non-empty string", self.refused(tag=""))
        self.assertIn("size must be a non-negative integer", self.refused(assets=[asset("a.deb", -1)]))
        self.assertIn("size must be a non-negative integer", self.refused(assets=[{**asset("a.deb"), "size": True}]))
        self.assertIn("appears twice", self.refused(assets=[asset("a.deb"), asset("a.deb")]))
        self.assertIn("does not start with YYYY-MM-DD", self.refused(published_at="yesterday"))

    def test_an_address_must_be_a_github_address_that_cannot_break_the_page(self) -> None:
        for url in ("https://example.com/a.deb", "http://github.com/a.deb", "https://github.com.evil.test/a.deb",
                    "https://github.com/a b.deb", 'https://github.com/a".deb', "https://github.com/a(1).deb", "//github.com/a.deb"):
            with self.subTest(url=url):
                self.assertIn("must be an https://github.com/ address", self.refused(assets=[{**asset("a.deb"), "url": url}]))
        self.assertIn("html_url", self.refused(html_url="https://example.com/tag/v1"))

    def test_a_file_that_is_missing_or_not_json_is_named(self) -> None:
        with self.assertRaises(hook.ReleaseDataError) as caught:
            hook.load_release(self.folder / "nothing.json")
        self.assertIn("does not exist", str(caught.exception))
        self.put("release.json", "{not json")
        with self.assertRaises(hook.ReleaseDataError) as caught:
            hook.load_release(self.folder / "release.json")
        self.assertIn("is not valid JSON", str(caught.exception))

    def test_a_rule_that_is_not_a_regular_expression_is_refused(self) -> None:
        self.put("download_rules.json", json.dumps({"rules": [{"match": "(", "platform": "A", "label": "b"}]}))
        with self.assertRaises(hook.ReleaseDataError) as caught:
            hook.load_rules(self.folder / "download_rules.json")
        self.assertIn("not a valid regular expression", str(caught.exception))

    def test_the_data_of_the_repository_loads_and_every_file_has_a_group(self) -> None:
        folder = REPO / hook.RELEASE_DIR
        release = hook.load_release(folder / hook.RELEASE_NAME)
        rules = hook.load_rules(folder / hook.RULES_NAME)
        html = hook.render_downloads(release, rules)
        self.assertNotIn("Other files", html, "a file of the recorded release fits no rule of download_rules.json")
        for found in release.assets:
            if not found.name.endswith(hook.SIGNATURE_SUFFIX):
                self.assertIn(f'<span class="vss-dl__name">{found.name}</span>', html)


class Downloads(unittest.TestCase):
    def test_exact_markup_with_signature(self) -> None:
        name = "vss-0.1.0-windows-x86_64-testsigned.exe"
        out = hook.render_downloads(release_of([asset(name, 43_900_000), asset(name + ".asc", 800)]), RULES)
        url = PREFIX + "download/v0.1.0-rc.3/" + name
        self.assertEqual(
            out,
            '<section class="vss-dl-group" aria-labelledby="dl-windows">\n'
            '<h2 id="dl-windows">Windows</h2>\n'
            '<table class="vss-dl">\n'
            '<caption class="icl-sr">Windows downloads</caption>\n'
            '<thead><tr><th scope="col">File</th><th scope="col">Size</th><th scope="col">Signature</th></tr></thead>\n'
            "<tbody>\n"
            f'<tr><th scope="row"><a href="{url}">Installer, with the test-signed driver</a>'
            f'<span class="vss-dl__name">{name}</span></th><td>43.9 MB</td>'
            f'<td><a href="{url}.asc" aria-label="Signature for {name}">.asc</a></td></tr>\n'
            "</tbody>\n</table>\n</section>",
        )

    def test_no_signature_cell_when_there_is_no_signature(self) -> None:
        out = hook.render_downloads(release_of([asset("vss_1_amd64.deb", 268_000)]), RULES)
        self.assertIn("<td>268 kB</td><td>None</td></tr>", out)
        self.assertNotIn(".asc", out)

    def test_a_signature_is_not_a_row(self) -> None:
        out = hook.render_downloads(release_of([asset("a.deb"), asset("a.deb.asc")]), RULES)
        self.assertEqual(out.count('<tr><th scope="row">'), 1)

    def test_a_signature_without_its_file_is_an_ordinary_file(self) -> None:
        out = hook.render_downloads(release_of([asset("lonely.deb.asc")]), RULES)
        self.assertIn('<h2 id="dl-other-files">Other files</h2>', out)
        self.assertIn('<span class="vss-dl__name">lonely.deb.asc</span>', out)

    def test_group_and_row_order(self) -> None:
        names = ["zzz.txt", "b.rpm", "a.rpm", "c.deb", "x-windows-x86_64.exe", "x-windows-x86_64-testsigned.exe", "aaa.txt"]
        out = hook.render_downloads(release_of([asset(n) for n in names]), RULES)
        headings = [line for line in out.splitlines() if line.startswith("<h2")]
        self.assertEqual(headings, ['<h2 id="dl-windows">Windows</h2>', '<h2 id="dl-linux">Linux</h2>', '<h2 id="dl-other-files">Other files</h2>'])
        shown = [part.split("<")[0] for part in out.split('<span class="vss-dl__name">')[1:]]
        self.assertEqual(shown, ["x-windows-x86_64-testsigned.exe", "x-windows-x86_64.exe", "c.deb", "a.rpm", "b.rpm", "aaa.txt", "zzz.txt"])

    def test_a_file_that_fits_no_rule_is_labelled_with_its_name(self) -> None:
        out = hook.render_downloads(release_of([asset("notes.txt")]), RULES)
        self.assertIn('>notes.txt</a><span class="vss-dl__name">notes.txt</span>', out)

    def test_empty_groups_are_left_out(self) -> None:
        out = hook.render_downloads(release_of([asset("a.deb")]), RULES)
        self.assertNotIn("Windows", out)
        self.assertNotIn("Other files", out)

    def test_names_are_escaped(self) -> None:
        out = hook.render_downloads(release_of([{"name": 'a"b&c.deb', "size": 1, "url": PREFIX + "x"}]), RULES)
        self.assertIn("a&quot;b&amp;c.deb", out)
        self.assertNotIn('a"b', out)

    def test_it_is_plain_html_with_no_script_and_only_github_links(self) -> None:
        out = hook.render_downloads(release_of([asset("a.deb"), asset("a.deb.asc"), asset("n.txt")]), RULES)
        self.assertNotIn("<script", out)
        self.assertNotIn("style=", out)
        for address in re.findall(r'href="([^"]*)"', out):
            self.assertTrue(address.startswith("https://github.com/"), address)


class Markers(Folder):
    def described(self) -> dict:
        release = hook.load_release(self.folder / "release.json")
        return hook.describe(release, hook.load_rules(self.folder / "download_rules.json"))

    def test_every_marker_is_replaced(self) -> None:
        text = ("Version <!-- releases:version -->, tag <!-- releases:tag -->, on <!-- releases:date -->; a <!-- releases:kind -->.\n"
                "[Notes](<!-- releases:url -->)\n\n<!-- releases:downloads -->\n")
        out = hook.fill(text, self.described())
        self.assertIn("Version 0.1.0-rc.3, tag v0.1.0-rc.3, on 12 November 2026; a release candidate.", out)
        self.assertIn(f"[Notes]({PREFIX}tag/v0.1.0-rc.3)", out)
        self.assertIn('<h2 id="dl-linux">Linux</h2>', out)
        self.assertNotIn("<!--", out)

    def test_spaces_inside_the_marker_do_not_matter(self) -> None:
        self.assertEqual(hook.fill("<!--releases:tag-->|<!--   releases:tag   -->", self.described()), "v0.1.0-rc.3|v0.1.0-rc.3")

    def test_an_unknown_marker_stops_the_build(self) -> None:
        with self.assertRaises(hook.ReleaseDataError) as caught:
            hook.fill("<!-- releases:versoin -->", self.described())
        self.assertIn("releases:versoin", str(caught.exception))
        self.assertIn("downloads", str(caught.exception))

    def test_other_comments_are_left_alone(self) -> None:
        self.assertEqual(hook.fill("<!-- a comment -->", self.described()), "<!-- a comment -->")

    def test_the_downloads_marker_on_its_own_line_gives_a_block_of_html(self) -> None:
        out = hook.fill("Before.\n\n<!-- releases:downloads -->\n\nAfter.\n", self.described())
        blocks = out.split("\n\n")
        self.assertEqual(blocks[0], "Before.")
        self.assertTrue(blocks[1].startswith("<section"), blocks[1][:30])
        self.assertTrue(blocks[1].endswith("</section>"))
        self.assertEqual(blocks[2], "After.\n")

    def test_a_value_from_the_data_cannot_add_markup(self) -> None:
        self.put("release.json", json.dumps(release_json(tag="v1<b>&", assets=[asset("a.deb")])))
        out = hook.fill("<!-- releases:tag -->|<!-- releases:version -->", self.described())
        self.assertEqual(out, "v1&lt;b&gt;&amp;|1&lt;b&gt;&amp;")

    def test_the_hook_functions(self) -> None:
        config = hook.on_config(self.config())
        self.assertEqual(config["extra"]["release"]["tag"], "v0.1.0-rc.3")
        self.assertTrue(config["extra"]["release"]["prerelease"])
        self.assertEqual([a["name"] for a in config["extra"]["release"]["assets"]], ["a.deb", "a.deb.asc"])
        out = hook.on_page_markdown("v <!-- releases:version -->", None, config, None)
        self.assertEqual(out, "v 0.1.0-rc.3")
        self.assertEqual(hook.on_page_markdown("no markers here", None, config, None), "no markers here")

    def test_a_bad_record_stops_the_build_at_the_start(self) -> None:
        self.put("release.json", "{}")
        with self.assertRaises(hook.ReleaseDataError):
            hook.on_config(self.config())


class FeedPublishing(Folder):
    def test_the_feed_is_copied_byte_for_byte(self) -> None:
        self.put("update-feed.json", FEED)
        hook.on_post_build(self.config())
        self.assertEqual((self.root / "site" / "updates" / "v1" / "feed.json").read_bytes(), FEED)
        self.assertEqual((self.folder / "update-feed.json").read_bytes(), FEED)

    def test_without_a_feed_the_site_has_no_updates_folder(self) -> None:
        hook.on_post_build(self.config())
        self.assertFalse((self.root / "site" / "updates").exists())

    def test_what_the_hook_says_is_a_plain_message_and_not_a_warning(self) -> None:
        with self.assertLogs("mkdocs.hooks.site_release", level="DEBUG") as logs:
            hook.on_post_build(self.config())
        self.assertEqual([record.levelno for record in logs.records], [logging.INFO])
        self.assertIn("so the site has no updates/v1/feed.json", logs.output[0])
        self.put("update-feed.json", FEED)
        with self.assertLogs("mkdocs.hooks.site_release", level="DEBUG") as logs:
            hook.on_post_build(self.config())
        self.assertEqual([record.levelno for record in logs.records], [logging.INFO])
        self.assertIn("copied unchanged to updates/v1/feed.json", logs.output[0])

    def test_a_second_build_gives_the_same_bytes(self) -> None:
        self.put("update-feed.json", FEED)
        hook.on_post_build(self.config())
        hook.on_post_build(self.config())
        self.assertEqual((self.root / "site" / "updates" / "v1" / "feed.json").read_bytes(), FEED)

    def test_publish_feed_says_whether_there_was_one(self) -> None:
        site = self.root / "out"
        self.assertFalse(hook.publish_feed(self.folder / "update-feed.json", site))
        self.assertFalse(site.exists())
        self.put("update-feed.json", FEED)
        self.assertTrue(hook.publish_feed(self.folder / "update-feed.json", site))


class DownloadsPage(unittest.TestCase):
    """docs/downloads.md, as the hook needs it."""

    def setUp(self) -> None:
        self.text = (REPO / "docs" / "downloads.md").read_text(encoding="utf-8")

    def test_it_uses_the_markers_the_hook_knows(self) -> None:
        used = set(re.findall(r"<!--\s*releases:([a-z]+)\s*-->", self.text))
        self.assertIn("downloads", used)
        self.assertLessEqual(used, hook.MARKERS)

    def test_it_is_a_front_page_with_the_verify_anchor(self) -> None:
        self.assertIn("template: marketing.html", self.text)
        self.assertIn("title: Downloads", self.text)
        self.assertIn("{ #verify }", self.text)

    def test_the_key_it_links_to_is_published_beside_it(self) -> None:
        self.assertTrue((REPO / "docs" / "signing-key.asc").is_file())
        self.assertIn("](signing-key.asc)", self.text)

    def test_no_page_uses_a_marker_the_hook_does_not_know(self) -> None:
        for page in sorted((REPO / "docs").glob("*.md")):
            for key in re.findall(r"<!--\s*releases:([a-z]+)\s*-->", page.read_text(encoding="utf-8")):
                self.assertIn(key, hook.MARKERS, f"{page.name} uses <!-- releases:{key} -->")


# -- the refresh ---------------------------------------------------------------------------------------------------------


def api_release(tag: str = "v0.2.0", feed: bool = False, draft: bool = False, **changes: object) -> dict:
    assets = [{"name": "b.deb", "size": 20, "id": 2, "browser_download_url": f"{PREFIX}download/{tag}/b.deb"},
              {"name": "a.deb", "size": 10, "browser_download_url": f"{PREFIX}download/{tag}/a.deb"}]
    if feed:
        assets.append({"name": "update-feed.json", "size": 99, "browser_download_url": f"{PREFIX}download/{tag}/update-feed.json"})
    entry: dict = {"tag_name": tag, "prerelease": "-" in tag, "draft": draft, "published_at": "2026-12-01T10:00:00Z",
                   "html_url": PREFIX + "tag/" + tag, "assets": assets, "body": "ignored"}
    entry.update(changes)
    return entry


class Refresh(Folder):
    def refresh(self, response: object, messages: list[str] | None = None, feed: bytes | None = None) -> list[str]:
        asked: list[str] = []

        def download(url: str) -> bytes:
            asked.append(url)
            return feed if feed is not None else b"the feed of " + url.split("/")[-2].encode()

        refresh_release.refresh(self.root, fetch=lambda repo: response, fetch_asset=download,
                                log=(messages.append if messages is not None else None))
        return asked

    def stored(self) -> bytes:
        return (self.folder / "update-feed.json").read_bytes()

    def record(self) -> bytes:
        return (self.folder / "release.json").read_bytes()

    def test_the_repository_is_found_in_mkdocs_yml(self) -> None:
        seen: list[str] = []
        refresh_release.refresh(self.root, fetch=lambda repo: seen.append(repo) or [api_release()], fetch_asset=lambda url: b"x")
        self.assertEqual(seen, [REPO_NAME])
        (self.root / "mkdocs.yml").write_text("repo_url: https://example.com/a/b\n", encoding="utf-8")
        with self.assertRaises(refresh_release.RefreshError):
            refresh_release.refresh(self.root, fetch=lambda repo: [api_release()])

    def test_release_json_has_the_expected_shape(self) -> None:
        self.refresh([api_release(), api_release(tag="v0.1.0")])
        expected = {"tag": "v0.2.0", "prerelease": False, "published_at": "2026-12-01T10:00:00Z", "html_url": PREFIX + "tag/v0.2.0",
                    "assets": [{"name": "a.deb", "size": 10, "url": PREFIX + "download/v0.2.0/a.deb"},
                               {"name": "b.deb", "size": 20, "url": PREFIX + "download/v0.2.0/b.deb"}]}
        self.assertEqual(self.record(), (json.dumps(expected, indent=1) + "\n").encode("utf-8"))
        self.assertNotIn(b"\r", self.record())

    def test_the_file_it_writes_is_one_the_hook_reads(self) -> None:
        self.refresh([api_release(tag="v0.3.0-rc.1")])
        release = hook.load_release(self.folder / "release.json")
        self.assertEqual((release.tag, release.prerelease), ("v0.3.0-rc.1", True))

    def test_a_draft_is_not_the_newest_release(self) -> None:
        self.refresh([api_release(tag="v9.0.0", draft=True), api_release(tag="v0.2.0")])
        self.assertIn(b'"tag": "v0.2.0"', self.record())

    def test_an_address_of_another_place_is_refused_and_nothing_changes(self) -> None:
        self.put("update-feed.json", FEED)
        before = self.record()
        foreign = "https://evil.test/releases/download/x.deb"
        cases = {
            "asset": api_release(assets=[{"name": "x.deb", "size": 1, "browser_download_url": foreign}]),
            "html_url": api_release(html_url="https://github.com/other/repo/releases/tag/v1"),
            "look-alike": api_release(html_url="https://github.com/example/site-evil/releases/tag/v1"),
            "scheme": api_release(html_url="http://github.com/example/site/releases/tag/v1"),
        }
        for label, entry in cases.items():
            with self.subTest(label), self.assertRaises(refresh_release.RefreshError) as caught:
                self.refresh([entry])
            self.assertIn(PREFIX, str(caught.exception))
            self.assertEqual(self.record(), before)
            self.assertEqual(self.stored(), FEED)

    def test_empty_and_malformed_responses_are_refused(self) -> None:
        for response in ([], {}, None, [None], [{"tag_name": "v1"}], [api_release(draft=True)]):
            with self.subTest(response=response), self.assertRaises(refresh_release.RefreshError):
                self.refresh(response)

    def test_a_network_failure_is_a_plain_error_and_changes_nothing(self) -> None:
        before = self.record()

        def broken(repo: str) -> object:
            raise OSError("no route to host")

        with self.assertRaises(refresh_release.RefreshError) as caught:
            refresh_release.refresh(self.root, fetch=broken)
        self.assertIn("no route to host", str(caught.exception))
        self.assertEqual(self.record(), before)

    def test_the_feed_of_the_newest_release_that_has_one(self) -> None:
        asked = self.refresh([api_release("v0.4.0"), api_release("v0.2.0", feed=True), api_release("v0.3.0-rc.1", feed=True),
                              api_release("v0.1.0", feed=True)])
        self.assertEqual(self.stored(), b"the feed of v0.3.0-rc.1")
        self.assertEqual(asked, [f"{PREFIX}download/v0.3.0-rc.1/update-feed.json"])

    def test_the_greatest_version_and_not_the_first_or_the_latest(self) -> None:
        self.refresh([api_release("v0.2.0", feed=True), api_release("v0.10.0-rc.1", feed=True), api_release("v0.9.0", feed=True)])
        self.assertEqual(self.stored(), b"the feed of v0.10.0-rc.1")
        self.refresh([api_release("v0.2.0-rc.1", feed=True), api_release("v0.2.0", feed=True), api_release("v0.2.0-rc.2", feed=True)])
        self.assertEqual(self.stored(), b"the feed of v0.2.0")

    def test_a_draft_and_a_tag_that_is_not_a_version_do_not_count(self) -> None:
        self.refresh([api_release("v9.0.0", feed=True, draft=True), api_release("nightly", feed=True), api_release("v0.2.0", feed=True)])
        self.assertEqual(self.stored(), b"the feed of v0.2.0")

    def test_the_feed_is_stored_as_it_came(self) -> None:
        for data in (FEED, b"[]"):
            self.refresh([api_release(feed=True)], feed=data)
            self.assertEqual(self.stored(), data)

    def test_no_release_with_a_feed_leaves_the_stored_file_alone_and_says_so(self) -> None:
        messages: list[str] = []
        self.refresh([api_release(feed=False)], messages)
        self.assertFalse((self.folder / "update-feed.json").exists())
        self.assertEqual(messages, ["no release has update-feed.json, so the stored feed is left as it is"])
        self.put("update-feed.json", FEED)
        self.refresh([api_release(feed=False)])
        self.assertEqual(self.stored(), FEED)
        self.assertIn(b'"tag": "v0.2.0"', self.record())

    def test_the_log_names_the_release(self) -> None:
        messages: list[str] = []
        self.refresh([api_release(feed=True)], messages)
        self.assertEqual(messages, ["update-feed.json of v0.2.0 is the update feed"])

    def test_a_feed_address_of_another_place_is_refused_and_nothing_changes(self) -> None:
        self.put("update-feed.json", FEED)
        before = self.record()
        entry = api_release(feed=True)
        entry["assets"][2]["browser_download_url"] = "https://evil.test/update-feed.json"
        with self.assertRaises(refresh_release.RefreshError) as caught:
            self.refresh([entry])
        self.assertIn(PREFIX, str(caught.exception))
        self.assertEqual(self.stored(), FEED)
        self.assertEqual(self.record(), before)

    def test_a_download_that_fails_changes_nothing(self) -> None:
        self.put("update-feed.json", FEED)
        before = self.record()

        def broken(url: str) -> bytes:
            raise OSError("connection reset")

        with self.assertRaises(refresh_release.RefreshError) as caught:
            refresh_release.refresh(self.root, fetch=lambda repo: [api_release(feed=True)], fetch_asset=broken)
        self.assertIn("connection reset", str(caught.exception))
        self.assertIn("v0.2.0", str(caught.exception))
        self.assertEqual(self.stored(), FEED)
        self.assertEqual(self.record(), before)

    def test_an_empty_or_large_feed_is_refused_and_the_limit_itself_is_fine(self) -> None:
        for data in (b"", b"x" * (refresh_release.MAX_FEED_BYTES + 1)):
            with self.subTest(size=len(data)), self.assertRaises(refresh_release.RefreshError):
                self.refresh([api_release(feed=True)], feed=data)
        self.assertFalse((self.folder / "update-feed.json").exists())
        data = b"x" * refresh_release.MAX_FEED_BYTES
        self.refresh([api_release(feed=True)], feed=data)
        self.assertEqual(self.stored(), data)
        self.assertEqual(refresh_release.MAX_FEED_BYTES, 64 * 1024)

    def test_the_command_reports_what_it_stored(self) -> None:
        out = io.StringIO()
        with unittest.mock.patch.object(refresh_release, "fetch_releases", return_value=[api_release(feed=True)]), \
                unittest.mock.patch.object(refresh_release, "download_feed", return_value=FEED), contextlib.redirect_stdout(out):
            code = refresh_release.main(["--root", str(self.root)])
        self.assertEqual(code, 0)
        self.assertEqual(out.getvalue(), "refresh_release: update-feed.json of v0.2.0 is the update feed\n"
                                         "refresh_release: wrote tools/docs/release/release.json\n")
        self.assertEqual(self.stored(), FEED)

    def test_the_command_fails_with_a_message_and_status_1(self) -> None:
        err = io.StringIO()
        with unittest.mock.patch.object(refresh_release, "fetch_releases", side_effect=OSError("offline")), contextlib.redirect_stderr(err):
            self.assertEqual(refresh_release.main(["--root", str(self.root)]), 1)
        self.assertIn("refresh_release: could not fetch the releases of example/site: offline", err.getvalue())


class Response(io.BytesIO):
    def __enter__(self) -> "Response":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


class Network(unittest.TestCase):
    """What the refresh sends. open_url is the one place that opens a connection, and here it is a fake."""

    def run_with(self, body: bytes, call, env: dict) -> dict:  # noqa: ANN001
        seen: dict = {}

        def fake(request, timeout):  # type: ignore[no-untyped-def]
            seen["request"], seen["timeout"] = request, timeout
            return Response(body)

        clean = {"GH_TOKEN": "", "GITHUB_TOKEN": ""}
        with unittest.mock.patch.dict(os.environ, {**clean, **env}), unittest.mock.patch.object(refresh_release, "open_url", fake):
            seen["result"] = call()
        seen["headers"] = {k.lower(): v for k, v in seen["request"].header_items()}
        return seen

    def test_the_api_request_and_its_token(self) -> None:
        cases = (({"GH_TOKEN": "s3cret"}, "Bearer s3cret"), ({"GITHUB_TOKEN": "other"}, "Bearer other"),
                 ({"GH_TOKEN": "first", "GITHUB_TOKEN": "second"}, "Bearer first"), ({}, None))
        for env, expected in cases:
            with self.subTest(env=sorted(env)):
                seen = self.run_with(b"[]", lambda: refresh_release.fetch_releases(REPO_NAME), env)
                self.assertEqual(seen["result"], [])
                self.assertEqual(seen["request"].full_url, f"https://api.github.com/repos/{REPO_NAME}/releases?per_page=100")
                self.assertEqual(seen["timeout"], 15)
                self.assertEqual(seen["headers"]["accept"], "application/vnd.github+json")
                self.assertEqual(seen["headers"]["user-agent"], refresh_release.USER_AGENT)
                self.assertEqual(seen["headers"].get("authorization"), expected)

    def test_the_download_sends_no_token_and_reads_a_limited_amount(self) -> None:
        url = f"{PREFIX}download/v0.2.0/update-feed.json"
        seen = self.run_with(b"x" * (refresh_release.MAX_FEED_BYTES + 10), lambda: refresh_release.download_feed(url), {"GH_TOKEN": "s3cret"})
        self.assertEqual(len(seen["result"]), refresh_release.MAX_FEED_BYTES + 1)
        self.assertEqual(seen["request"].full_url, url)
        self.assertEqual(seen["timeout"], 15)
        self.assertNotIn("authorization", seen["headers"])

    def test_an_answer_over_the_limit_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            refresh_release.read_limited(Response(b"x" * 11), 10, "the answer")
        self.assertEqual(refresh_release.read_limited(Response(b"x" * 10), 10, "the answer"), b"x" * 10)

    def test_the_token_is_not_in_what_an_error_says(self) -> None:
        def broken(request, timeout):  # type: ignore[no-untyped-def]
            raise urllib.error.URLError("connection refused")

        with unittest.mock.patch.dict(os.environ, {"GH_TOKEN": "s3cret-value"}), unittest.mock.patch.object(refresh_release, "open_url", broken):
            with self.assertRaises(refresh_release.RefreshError) as caught:
                refresh_release.refresh(Path("."), repo=REPO_NAME)
        self.assertNotIn("s3cret-value", str(caught.exception))

    def test_only_https_is_opened(self) -> None:
        with self.assertRaises(urllib.error.URLError):
            refresh_release.open_url(urllib.request.Request("http://api.github.com/x"), 1)


class Redirects(unittest.TestCase):
    def follow(self, start: str, target: str, token: bool = True):  # noqa: ANN201
        request = urllib.request.Request(start, headers={"Authorization": "Bearer s3cret"} if token else {})
        return refresh_release.SafeRedirect().redirect_request(request, io.BytesIO(), 302, "Found", {}, target)

    def test_github_hosts_are_allowed(self) -> None:
        for target in ("https://github.com/x/y", "https://api.github.com/repositories/1/releases",
                       "https://objects.githubusercontent.com/a?sig=1", "https://release-assets.githubusercontent.com/a"):
            with self.subTest(target=target):
                self.assertTrue(refresh_release.host_allowed(target))
                self.assertIsNotNone(self.follow("https://github.com/a/b", target))

    def test_other_hosts_and_http_are_refused(self) -> None:
        for target in ("https://example.com/a", "http://github.com/a", "https://githubusercontent.com.evil.test/a",
                       "https://evilgithubusercontent.com/a", "https://github.com.evil.test/a", "ftp://github.com/a"):
            with self.subTest(target=target):
                self.assertFalse(refresh_release.host_allowed(target))
                with self.assertRaises(urllib.error.URLError):
                    self.follow("https://github.com/a/b", target)

    def test_a_refused_redirect_does_not_show_the_address(self) -> None:
        with self.assertRaises(urllib.error.URLError) as caught:
            self.follow("https://github.com/a", "https://example.com/path?sig=secret")
        self.assertIn("example.com", str(caught.exception))
        self.assertNotIn("secret", str(caught.exception))
        self.assertNotIn("/path", str(caught.exception))

    def test_the_token_does_not_go_to_another_host(self) -> None:
        moved = self.follow("https://api.github.com/repos/a/b/releases", "https://objects.githubusercontent.com/a")
        self.assertNotIn("Authorization", moved.headers)
        same = self.follow("https://api.github.com/repos/a/b/releases", "https://api.github.com/repositories/1/releases")
        self.assertEqual(same.headers.get("Authorization"), "Bearer s3cret")

    def test_a_relative_redirect_stays_on_the_host(self) -> None:
        moved = self.follow("https://api.github.com/repos/a/b/releases", "/repositories/1/releases")
        self.assertEqual(moved.full_url, "https://api.github.com/repositories/1/releases")


if __name__ == "__main__":
    unittest.main()
