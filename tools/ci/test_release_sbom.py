#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for release_sbom.py.

    python3 -m unittest discover -s tools/ci -p "test_release_sbom.py" -v

They read the real third_party.json and the files that pin the versions, so a component added
or a version moved without the document following fails here.
"""

from __future__ import annotations

import json
import re
import shutil
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import release_sbom  # noqa: E402

PINS = (
    "third_party.json",
    ".github/toolchain-versions.json",
    "tools/fetch_boost.py",
    "cmake/VssVirtualSerial.cmake",
    "third_party/monocypher/README.md",
)
WHEN = datetime(2026, 11, 15, 1, 2, 3, tzinfo=timezone.utc)
THIRD_PARTY = json.loads((ROOT / "third_party.json").read_text(encoding="utf-8"))


def document(root: Path = ROOT, tag: str = "v0.1.0-rc.5") -> dict:
    return release_sbom.document(root, tag.lstrip("v"), tag, WHEN)


def names(doc: dict) -> list[str]:
    return [p["name"] for p in doc["packages"]]


def copy_pins() -> Path:
    root = Path(tempfile.mkdtemp())
    for relative in PINS:
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / relative, target)
    return root


class Contents(unittest.TestCase):
    def test_every_shipped_component_and_font_is_a_package_with_a_version(self):
        doc = document()
        by_name = {p["name"]: p for p in doc["packages"]}
        shipped = [c for c in THIRD_PARTY["components"] if not c["condition"]]
        self.assertGreaterEqual(len(shipped), 4)
        for component in shipped:
            package = by_name.get(component["name"])
            self.assertIsNotNone(package, component["name"])
            self.assertTrue(package["versionInfo"], component["name"])
            self.assertEqual(package["licenseDeclared"], component["spdx"])
        for font in THIRD_PARTY["fonts"]:
            self.assertEqual(by_name[font["family"]]["versionInfo"], font["version"])
            self.assertEqual(by_name[font["family"]]["licenseDeclared"], font["spdx"])

    def test_what_the_packages_do_not_contain_is_left_out(self):
        doc = document()
        for component in THIRD_PARTY["components"]:
            if component["condition"]:
                self.assertNotIn(component["name"], names(doc), component["name"])
        self.assertNotIn("Tracy", names(doc))
        self.assertNotIn("com0com", " ".join(names(doc)))

    def test_the_versions_are_the_ones_the_build_pins(self):
        by_name = {p["name"]: p["versionInfo"] for p in document()["packages"]}
        pins = json.loads((ROOT / ".github/toolchain-versions.json").read_text(encoding="utf-8"))
        self.assertEqual(by_name["Qt 6"], pins["qt"])
        boost = re.search(r'^DEFAULT_VERSION = "([0-9.]+)"', (ROOT / "tools/fetch_boost.py").read_text(encoding="utf-8"), re.M)
        self.assertEqual(by_name["Boost"], boost.group(1))
        monocypher = (ROOT / "third_party/monocypher/README.md").read_text(encoding="utf-8")
        self.assertIn(f"Monocypher](https://monocypher.org) {by_name['Monocypher']},", monocypher)
        library = (ROOT / "cmake/VssVirtualSerial.cmake").read_text(encoding="utf-8")
        self.assertIn(f'"v{by_name["Boost.VirtualSerial"]}"', library)

    def test_the_scanners_find_qt_and_boost_by_cpe_and_purl(self):
        by_name = {p["name"]: p for p in document()["packages"]}
        for name in ("Qt 6", "Boost"):
            refs = {r["referenceType"]: r["referenceLocator"] for r in by_name[name]["externalRefs"]}
            self.assertIn(by_name[name]["versionInfo"], refs["cpe23Type"])
            self.assertIn(by_name[name]["versionInfo"], refs["purl"])

    def test_the_program_is_described_and_contains_each_package(self):
        doc = document()
        program = doc["packages"][0]
        self.assertEqual(program["name"], "virtual-serial-studio")
        self.assertEqual(program["versionInfo"], "0.1.0-rc.5")
        self.assertEqual(program["licenseDeclared"], "GPL-3.0-or-later")
        # The packages carry Qt Canvas Painter, which is version 3 only, so that is what is concluded.
        self.assertEqual(program["licenseConcluded"], "GPL-3.0-only")
        self.assertIn("Qt Canvas Painter", program["licenseComments"])
        relations ={(r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"]) for r in doc["relationships"]}
        self.assertIn(("SPDXRef-DOCUMENT", "DESCRIBES", program["SPDXID"]), relations)
        for package in doc["packages"][1:]:
            self.assertIn((program["SPDXID"], "CONTAINS", package["SPDXID"]), relations)


class Shape(unittest.TestCase):
    def test_it_is_an_spdx_2_3_document_with_the_required_fields(self):
        doc = document()
        self.assertEqual(doc["spdxVersion"], "SPDX-2.3")
        self.assertEqual(doc["dataLicense"], "CC0-1.0")
        self.assertEqual(doc["SPDXID"], "SPDXRef-DOCUMENT")
        self.assertEqual(doc["creationInfo"]["created"], "2026-11-15T01:02:03Z")
        self.assertTrue(doc["creationInfo"]["creators"])
        for package in doc["packages"]:
            for key in ("SPDXID", "name", "downloadLocation", "filesAnalyzed", "licenseDeclared", "copyrightText"):
                self.assertIn(key, package, package["name"])
            self.assertRegex(package["SPDXID"], r"^SPDXRef-[A-Za-z0-9.-]+$")

    def test_the_identifiers_are_unique(self):
        ids = [p["SPDXID"] for p in document()["packages"]]
        self.assertEqual(len(ids), len(set(ids)))

    def test_each_release_has_a_namespace_of_its_own(self):
        self.assertNotEqual(document(tag="v0.1.0-rc.5")["documentNamespace"], document(tag="v0.1.0-rc.6")["documentNamespace"])

    def test_the_file_is_written_as_json(self):
        out = Path(tempfile.mkdtemp()) / "dist" / "sbom.spdx.json"
        self.assertEqual(release_sbom.main(["--version", "0.1.0-rc.5", "--tag", "v0.1.0-rc.5", "--out", str(out)]), 0)
        self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["spdxVersion"], "SPDX-2.3")


def pinned_tag(text):
    """The library tag the real cmake file pins, so a release bump does not break these tests."""
    return re.search(r'set\(VSS_VIRTUAL_SERIAL_GIT_TAG "(v[^"]+)"', text).group(1)


class Pins(unittest.TestCase):
    def test_a_new_shipped_component_stops_the_script_until_its_version_is_found(self):
        root = copy_pins()
        data = json.loads((root / "third_party.json").read_text(encoding="utf-8"))
        data["components"].append({"notice": "Newcomer", "name": "Newcomer", "spdx": "MIT", "shipped": "x", "condition": ""})
        (root / "third_party.json").write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaises(SystemExit) as caught:
            document(root)
        self.assertIn("Newcomer", str(caught.exception))

    def test_a_component_only_a_profiling_build_has_needs_no_version_source(self):
        root = copy_pins()
        data = json.loads((root / "third_party.json").read_text(encoding="utf-8"))
        data["components"].append({"notice": "Probe", "name": "Probe", "spdx": "MIT", "shipped": "x", "condition": "profiling"})
        (root / "third_party.json").write_text(json.dumps(data), encoding="utf-8")
        self.assertNotIn("Probe", names(document(root)))

    def test_a_moved_qt_pin_moves_the_document(self):
        root = copy_pins()
        pins = json.loads((root / ".github/toolchain-versions.json").read_text(encoding="utf-8"))
        pins["qt"] = "6.99.1"
        (root / ".github/toolchain-versions.json").write_text(json.dumps(pins), encoding="utf-8")
        self.assertIn("6.99.1", {p["versionInfo"] for p in document(root)["packages"]})

    def test_a_library_pinned_by_commit_ignores_another_tag_in_a_comment_nearby(self):
        root = copy_pins()
        path = root / "cmake/VssVirtualSerial.cmake"
        text = path.read_text(encoding="utf-8")
        commit = "0123456789abcdef0123456789abcdef01234567"
        text = text.replace(
            f'set(VSS_VIRTUAL_SERIAL_GIT_TAG "{pinned_tag(text)}"',
            f'# v0.0.9 was the one before\n# the pin moves with each release\nset(VSS_VIRTUAL_SERIAL_GIT_TAG "{commit}"',
        )
        path.write_text(text, encoding="utf-8")
        with self.assertRaises(SystemExit) as caught:
            document(root)
        self.assertIn("needs its tag in a comment", str(caught.exception))

    def test_a_tag_at_the_end_of_the_line_of_the_pin_counts(self):
        root = copy_pins()
        path = root / "cmake/VssVirtualSerial.cmake"
        commit = "0123456789abcdef0123456789abcdef01234567"
        original = path.read_text(encoding="utf-8")
        tag = pinned_tag(original)
        text = original.replace(
            f'set(VSS_VIRTUAL_SERIAL_GIT_TAG "{tag}" CACHE STRING',
            f'set(VSS_VIRTUAL_SERIAL_GIT_TAG "{commit}" # {tag}',
        )
        path.write_text(text, encoding="utf-8")
        library = next(p for p in document(root)["packages"] if p["name"] == "Boost.VirtualSerial")
        self.assertEqual(library["versionInfo"], tag.lstrip("v"))

    def test_a_library_pinned_by_commit_takes_its_version_from_the_tag_beside_it(self):
        root = copy_pins()
        path = root / "cmake/VssVirtualSerial.cmake"
        text = path.read_text(encoding="utf-8")
        commit = "0123456789abcdef0123456789abcdef01234567"
        tag = pinned_tag(text)
        text = text.replace(
            f'set(VSS_VIRTUAL_SERIAL_GIT_TAG "{tag}"',
            f'# {tag}\nset(VSS_VIRTUAL_SERIAL_GIT_TAG "{commit}"',
        )
        path.write_text(text, encoding="utf-8")
        library = next(p for p in document(root)["packages"] if p["name"] == "Boost.VirtualSerial")
        self.assertEqual(library["versionInfo"], tag.lstrip("v"))
        self.assertTrue(library["downloadLocation"].endswith("@" + commit))


if __name__ == "__main__":
    unittest.main()
