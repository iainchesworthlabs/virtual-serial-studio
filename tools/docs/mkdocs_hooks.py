# SPDX-License-Identifier: GPL-3.0-or-later
"""MkDocs hook: links that leave docs/ become links to the file on GitHub.

The pages are written for GitHub's file view, where `[README](../README.md)` works. On the built site
there is no README.md beside the pages, so MkDocs cannot resolve the link. This hook rewrites a
relative link whose target is a file or folder of the repository that is not a page of the site to
`<repo_url>/blob/develop/<path>` (`tree/develop` for a folder). A target that does not exist is left alone,
so MkDocs' own validation (mkdocs.yml, `validation`) reports it and the strict build fails.

A Markdown file of docs/ that the site does not publish (`exclude_docs` in mkdocs.yml: the pages for the people who work on
the project) is treated the same way, so a published page can link to it and the link goes to the file on GitHub.

Fenced code is not touched. The hook also adds the application's icon to the site (`on_files`).
"""

import os
import posixpath
import re

from mkdocs.structure.files import File

# develop and not main: main only moves when a final release is published, and a page must link to the file as it is
# in the trunk that the pages are built from.
BRANCH = "develop"

# [text](target) and ![alt](target); the target has no spaces or parentheses in this documentation.
_LINK = re.compile(r"(\]\()([^)\s#]*)(#[^)\s]*)?(\))")
_SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")
_FENCE = re.compile(r"^\s*(```|~~~)")


def _rewrite(line, page_dir, docs_dir, repo_root, repo_url, page_uris):
    def replace(match):
        opening, target, anchor, closing = match.groups()
        if not target or target.startswith("/") or _SCHEME.match(target):
            return match.group(0)
        relative = posixpath.normpath(posixpath.join(page_dir, target))
        if relative in page_uris:
            return match.group(0)
        on_disk = os.path.normpath(os.path.join(docs_dir, *relative.split("/")))
        in_repo = os.path.relpath(on_disk, repo_root).replace(os.sep, "/")
        if in_repo.startswith(".."):
            return match.group(0)
        if os.path.isdir(on_disk):
            kind = "tree"
        elif os.path.isfile(on_disk) and not on_disk.startswith(docs_dir + os.sep):
            kind = "blob"
        elif os.path.isfile(on_disk) and relative.endswith(".md"):
            # A Markdown file inside docs/ that the site does not publish (exclude_docs): the link goes to the file on GitHub.
            kind = "blob"
        elif os.path.isfile(on_disk):
            # An asset inside docs/ (a picture, the public key): MkDocs publishes it and resolves the link.
            return match.group(0)
        else:
            return match.group(0)
        return f"{opening}{repo_url}/{kind}/{BRANCH}/{in_repo}{anchor or ''}{closing}"

    return _LINK.sub(replace, line)


def on_files(files, config):
    """The site's logo and favicon (mkdocs.yml, theme) are the application's icon.

    It is taken from the master file of the application's icon, so there is one copy of it, and added to the
    site as icon.svg, the address the company site's contract gives an app's icon
    (/virtual-serial-studio/icon.svg). MkDocs only publishes what is under docs/.
    """
    repo_root = os.path.dirname(os.path.normpath(config["docs_dir"]))
    icon = os.path.join(repo_root, "apps", "vss_gui", "assets", "icon", "vss_gui.svg")
    files.append(File.generated(config, "icon.svg", abs_src_path=icon))
    return files


def on_page_markdown(markdown, page, config, files):
    docs_dir = os.path.normpath(config["docs_dir"])
    repo_root = os.path.dirname(docs_dir)
    repo_url = config["repo_url"].rstrip("/")
    page_dir = posixpath.dirname(page.file.src_uri)
    page_uris = {f.src_uri for f in files if f.src_uri.endswith(".md") and not f.inclusion.is_excluded()}

    out = []
    fenced = False
    for line in markdown.split("\n"):
        if _FENCE.match(line):
            fenced = not fenced
            out.append(line)
        elif fenced:
            out.append(line)
        else:
            out.append(_rewrite(line, page_dir, docs_dir, repo_root, repo_url, page_uris))
    return "\n".join(out)
