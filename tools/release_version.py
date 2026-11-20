# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""The versions that the update feed allows, and which of two is greater.

Both ``update_feed.py`` (which writes and checks the feed) and
``site_build.py`` (which picks the newest release that has a feed) use these
rules, so they are here and neither tool imports the other for them. Standard
library only.
"""

from __future__ import annotations

import re
from typing import Any, Optional, Tuple

MAX_VERSION_LENGTH = 64
MAX_VERSION_NUMBER = 999999
MAX_PRERELEASE_IDENTIFIERS = 16

_NUMBER = re.compile(r"(0|[1-9][0-9]*)")
_IDENTIFIER = re.compile(r"[0-9A-Za-z-]+")
_DIGITS = re.compile(r"[0-9]+")


def _parse_version(text: Any) -> Tuple[Tuple[int, int, int], Tuple[str, ...]]:
    """The three numbers and the pre-release identifiers of a version.
    Raises ``ValueError`` with the reason when it is not one the feed allows."""
    if not isinstance(text, str):
        raise ValueError("must be a string")
    if not 1 <= len(text) <= MAX_VERSION_LENGTH:
        raise ValueError("must be 1 to %d characters" % MAX_VERSION_LENGTH)
    core, dash, pre = text.partition("-")
    parts = core.split(".")
    if len(parts) != 3 or any(_NUMBER.fullmatch(p) is None for p in parts):
        raise ValueError(
            "must be a version such as 0.2.0 or 0.3.0-rc.1: three numbers "
            "joined by dots, no leading zero, no leading v and no build "
            "metadata"
        )
    if any(int(p) > MAX_VERSION_NUMBER for p in parts):
        raise ValueError("has a number above %d" % MAX_VERSION_NUMBER)
    numbers = (int(parts[0]), int(parts[1]), int(parts[2]))
    if not dash:
        return numbers, ()
    identifiers = pre.split(".")
    if len(identifiers) > MAX_PRERELEASE_IDENTIFIERS:
        raise ValueError(
            "has more than %d pre-release identifiers" % MAX_PRERELEASE_IDENTIFIERS
        )
    for identifier in identifiers:
        if _IDENTIFIER.fullmatch(identifier) is None:
            raise ValueError(
                "has a pre-release identifier that is empty or holds a "
                "character other than letters, digits and hyphen"
            )
        if _DIGITS.fullmatch(identifier) and _NUMBER.fullmatch(identifier) is None:
            raise ValueError("has a numeric pre-release identifier with a leading zero")
    if pre == "dev":
        raise ValueError("is a development version, which no feed may name")
    return numbers, tuple(identifiers)


def version_problem(text: Any) -> Optional[str]:
    """Why ``text`` is not a version the feed allows, or ``None``."""
    try:
        _parse_version(text)
    except ValueError as error:
        return str(error)
    return None


def is_version(text: Any) -> bool:
    return version_problem(text) is None


def compare_versions(first: str, second: str) -> int:
    """-1, 0 or 1: the precedence of two versions as Semantic Versioning
    2.0.0 defines it (item 11). Both must be versions the feed allows."""
    numbers_a, pre_a = _parse_version(first)
    numbers_b, pre_b = _parse_version(second)
    if numbers_a != numbers_b:
        return -1 if numbers_a < numbers_b else 1
    if pre_a == pre_b:
        return 0
    if not pre_a:
        return 1
    if not pre_b:
        return -1
    for left, right in zip(pre_a, pre_b):
        if left == right:
            continue
        left_numeric = _DIGITS.fullmatch(left) is not None
        right_numeric = _DIGITS.fullmatch(right) is not None
        if left_numeric and right_numeric:
            return -1 if int(left) < int(right) else 1
        if left_numeric != right_numeric:
            return -1 if left_numeric else 1
        return -1 if left < right else 1
    if len(pre_a) == len(pre_b):
        return 0
    return -1 if len(pre_a) < len(pre_b) else 1
