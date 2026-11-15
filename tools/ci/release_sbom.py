#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the SPDX software bill of materials of a release.

    python3 tools/ci/release_sbom.py --version 0.1.0-rc.5 --tag v0.1.0-rc.5 --out dist/sbom.spdx.json

The document lists what the packages hold: the program itself and each component of
third_party.json that is shipped or compiled in (an empty `condition`), with the versions
the repository pins, and each bundled font. A component that only a profiling build has
(`condition` not empty) is left out, because the release packages do not contain it. A
scan of the source tree finds the GitHub Actions and the test tools instead, which no
package holds.

The versions come from the files that pin them, so they cannot drift from the build:

    Qt                   .github/toolchain-versions.json  ("qt")
    Boost                tools/fetch_boost.py             (DEFAULT_VERSION)
    Boost.VirtualSerial  cmake/VssVirtualSerial.cmake     (VSS_VIRTUAL_SERIAL_GIT_TAG)
    Monocypher           third_party/monocypher/README.md
    fonts                third_party.json                 ("version")

A component in third_party.json that has no entry in COMPONENT_SOURCES stops the script, so
a new component cannot be left out of the document by accident.

release.yml hands the script to the shared publish workflow as `pre-release-script`, through
tools/ci/release_prepare.sh, and the file as `spdx-sbom`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

REPOSITORY = "iainchesworthlabs/virtual-serial-studio"
PROGRAM = "virtual-serial-studio"
PROGRAM_LICENCE = "GPL-3.0-or-later"
SUPPLIER = "Organization: Iain Chesworth Labs"
CREATOR_TOOL = "Tool: tools/ci/release_sbom.py"

# The reference scanners match a component by its CPE or its package URL.
CPE = {
    "Qt": "cpe:2.3:a:qt:qt:{version}:*:*:*:*:*:*:*",
    "Boost": "cpe:2.3:a:boost:boost:{version}:*:*:*:*:*:*:*",
}
PURL = {
    "Qt": "pkg:generic/qt@{version}",
    "Boost": "pkg:generic/boost@{version}",
    "Boost.VirtualSerial": "pkg:github/iainchesworthlabs/virtual-serial@{tag}",
    "Monocypher": "pkg:generic/monocypher@{version}",
}
HOMEPAGE = {
    "Qt": "https://www.qt.io",
    "Boost": "https://www.boost.org",
    "Boost.VirtualSerial": "https://github.com/iainchesworthlabs/virtual-serial",
    "Monocypher": "https://monocypher.org",
}


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


def find(pattern: str, text: str, what: str) -> re.Match[str]:
    found = re.search(pattern, text, re.MULTILINE)
    if not found:
        raise SystemExit(f"release_sbom: cannot find {what}")
    return found


def qt_version(root: Path) -> dict[str, str]:
    return {"version": json.loads(read(root, ".github/toolchain-versions.json"))["qt"]}


def boost_version(root: Path) -> dict[str, str]:
    version = find(r'^DEFAULT_VERSION = "([0-9.]+)"', read(root, "tools/fetch_boost.py"), "DEFAULT_VERSION in tools/fetch_boost.py").group(1)
    underscored = version.replace(".", "_")
    return {
        "version": version,
        "download": f"https://archives.boost.io/release/{version}/source/boost_{underscored}.tar.bz2",
    }


def virtual_serial_version(root: Path) -> dict[str, str]:
    text = read(root, "cmake/VssVirtualSerial.cmake")
    ref = find(r'set\(VSS_VIRTUAL_SERIAL_GIT_TAG "([^"]+)"', text, "VSS_VIRTUAL_SERIAL_GIT_TAG in cmake/VssVirtualSerial.cmake").group(1)
    repository = find(r'"(https://github\.com/[^"]+?)(?:\.git)?"', text[text.index("VSS_VIRTUAL_SERIAL_GIT_REPO") :], "the library repository").group(1)
    if re.fullmatch(r"[0-9a-f]{40}", ref):
        # A commit is pinned. Its tag is the one written as a comment on the line above the set() or
        # at the end of its line, such as `# v0.1.0-beta.2`; any other tag nearby is not looked at.
        lines = text.splitlines()
        index = next(i for i, line in enumerate(lines) if f'"{ref}"' in line)
        tag_form = r"#\s*(v[0-9]+\.[0-9]+\.[0-9]+[0-9A-Za-z.-]*)\s*"
        above = re.fullmatch(r"\s*" + tag_form, lines[index - 1]) if index else None
        beside = re.search(tag_form + "$", lines[index])
        found = above or beside
        if not found:
            raise SystemExit(
                "release_sbom: the commit pinned in cmake/VssVirtualSerial.cmake needs its tag in a comment, "
                "such as `# v0.1.0-beta.2`, on the line above or at the end of the line"
            )
        tag_name = found.group(1)
    else:
        tag_name = ref
    return {
        "version": tag_name.lstrip("v"),
        "tag": tag_name,
        "download": f"git+{repository}@{ref}",
    }


def monocypher_version(root: Path) -> dict[str, str]:
    version = find(r"\[Monocypher\]\([^)]*\)\s+([0-9]+\.[0-9]+\.[0-9]+)", read(root, "third_party/monocypher/README.md"), "the Monocypher version").group(1)
    return {"version": version, "download": f"https://monocypher.org/download/monocypher-{version}.tar.gz"}


# third_party.json "notice" -> where its version is pinned.
COMPONENT_SOURCES = {
    "Qt": qt_version,
    "Boost": boost_version,
    "Boost.VirtualSerial": virtual_serial_version,
    "Monocypher": monocypher_version,
}


def spdx_id(*parts: str) -> str:
    return "SPDXRef-" + re.sub(r"[^A-Za-z0-9.-]+", "-", "-".join(parts)).strip("-")


def component_package(entry: dict, pinned: dict[str, str]) -> dict:
    name = entry["notice"]
    refs = []
    if name in CPE:
        refs.append({"referenceCategory": "SECURITY", "referenceType": "cpe23Type", "referenceLocator": CPE[name].format(**pinned)})
    if name in PURL:
        refs.append({"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl", "referenceLocator": PURL[name].format(**pinned)})
    return {
        "SPDXID": spdx_id("Package", name),
        "name": entry["name"],
        "versionInfo": pinned["version"],
        "supplier": "NOASSERTION",
        "downloadLocation": pinned.get("download", "NOASSERTION"),
        "homepage": HOMEPAGE.get(name, "NOASSERTION"),
        "filesAnalyzed": False,
        "licenseConcluded": "NOASSERTION",
        "licenseDeclared": entry["spdx"],
        "copyrightText": "NOASSERTION",
        "comment": entry["shipped"],
        **({"externalRefs": refs} if refs else {}),
    }


def font_package(font: dict) -> dict:
    return {
        "SPDXID": spdx_id("Font", font["family"]),
        "name": font["family"],
        "versionInfo": font["version"],
        "supplier": "NOASSERTION",
        "downloadLocation": font["url"],
        "homepage": font["url"],
        "filesAnalyzed": False,
        "licenseConcluded": "NOASSERTION",
        "licenseDeclared": font["spdx"],
        "copyrightText": "NOASSERTION",
        "comment": "A font file bundled with the program.",
    }


def packages_of(root: Path) -> list[dict]:
    """The packages inside the release: every shipped component, then every bundled font."""
    data = json.loads(read(root, "third_party.json"))
    packages = []
    for entry in data["components"]:
        if entry.get("condition"):
            continue
        source = COMPONENT_SOURCES.get(entry["notice"])
        if source is None:
            raise SystemExit(
                f"release_sbom: third_party.json lists {entry['notice']!r} as shipped, and COMPONENT_SOURCES in "
                "tools/ci/release_sbom.py does not say where its version is pinned"
            )
        packages.append(component_package(entry, source(root)))
    packages += [font_package(font) for font in data["fonts"]]
    return packages


def document(root: Path, version: str, tag: str, created: datetime) -> dict:
    program = {
        "SPDXID": spdx_id("Package", PROGRAM),
        "name": PROGRAM,
        "versionInfo": version,
        "supplier": SUPPLIER,
        "downloadLocation": f"git+https://github.com/{REPOSITORY}@{tag}",
        "homepage": f"https://github.com/{REPOSITORY}",
        "filesAnalyzed": False,
        "licenseConcluded": PROGRAM_LICENCE,
        "licenseDeclared": PROGRAM_LICENCE,
        "copyrightText": "NOASSERTION",
        "externalRefs": [
            {"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl", "referenceLocator": f"pkg:github/{REPOSITORY}@{tag}"}
        ],
    }
    packages = packages_of(root)
    names = [program["SPDXID"]] + [p["SPDXID"] for p in packages]
    if len(set(names)) != len(names):
        raise SystemExit("release_sbom: two packages have the same SPDX identifier")
    relationships = [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": program["SPDXID"]}]
    relationships += [
        {"spdxElementId": program["SPDXID"], "relationshipType": "CONTAINS", "relatedSpdxElement": p["SPDXID"]} for p in packages
    ]
    # The namespace has to be unique to this document; the tag and the content make it so.
    fingerprint = hashlib.sha256(json.dumps([program, packages], sort_keys=True).encode()).hexdigest()[:16]
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{PROGRAM}-{version}",
        "documentNamespace": f"https://github.com/{REPOSITORY}/spdx/{tag}-{fingerprint}",
        "creationInfo": {
            "created": created.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "creators": [CREATOR_TOOL, SUPPLIER],
            "comment": "Lists what the release packages hold, from third_party.json and the files that pin each version.",
        },
        "packages": [program] + packages,
        "relationships": relationships,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--version", required=True, help="the release version without the v, such as 0.1.0-rc.5")
    parser.add_argument("--tag", required=True, help="the tag, such as v0.1.0-rc.5")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)

    doc = document(args.root, args.version, args.tag, datetime.now(timezone.utc))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"{args.out}: {len(doc['packages'])} packages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
