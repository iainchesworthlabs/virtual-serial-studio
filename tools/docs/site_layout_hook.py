# SPDX-License-Identifier: GPL-3.0-or-later
"""MkDocs hook: where each page of docs/ is on the site.

The files of docs/ stay where they are, because the code, the tests, CONTRIBUTING.md and the other pages cite them by path.
The site has its own layout: the user's guide is /virtual-serial-studio/guide/ (the file user-guide.md in the repository), the
other pages about using the studio are under guide/, and the pages for programmers are under developers/.
tools/docs/site_layout.json says, for each page of docs/, the address it has on the site, or `null` when it is published on
GitHub only. This hook applies that table to the files MkDocs found, and refuses to build when the table and the rest of the
configuration disagree, so a page cannot be lost quietly:

- a file in the table that is not in docs/;
- a page in the table that has an address ("new") but is excluded from the site (`exclude_docs` in mkdocs.yml), or one that has
  none (`null`: published on GitHub only) and is not excluded.

A page of docs/ that the table does not name is at the address its file name gives. tools/ci/check_site_layout.py checks the
built site against the same table and fails for a page that the table does not name. docs/ci.md, "The site layout".
"""

from __future__ import annotations

import json
from pathlib import Path

LAYOUT = Path(__file__).resolve().parent / "site_layout.json"


def load() -> list[dict]:
    return json.loads(LAYOUT.read_text(encoding="utf-8"))["pages"]


def on_files(files, config):  # noqa: ANN001, ANN201 (MkDocs passes its own types)
    problems: list[str] = []
    for entry in load():
        src, new = entry["src"], entry["new"]
        file = files.get_file_from_path(src)
        if file is None:
            problems.append(f"site_layout.json: {src} is not in docs/")
            continue
        if new is None:
            if not file.inclusion.is_excluded():
                problems.append(f"site_layout.json: {src} has no address on the site (null) but mkdocs.yml does not exclude it")
            continue
        if file.inclusion.is_excluded():
            problems.append(f"site_layout.json: {src} has the address {new!r} but mkdocs.yml excludes it (exclude_docs)")
            continue
        file.dest_uri = new + "index.html"
        # MkDocs worked these out from the old dest_uri and keeps them until they are asked for again.
        for cached in ("url", "abs_dest_path"):
            file.__dict__.pop(cached, None)
    if problems:
        raise ValueError("\n".join(problems))
    return files
