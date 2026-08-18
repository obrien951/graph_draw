"""Glob matching for repo-relative paths.

One small module so every other part of the harness (scope fence, ignore
lists, context selection) matches paths the same way.  Supports ``*``, ``?``,
``[...]`` and ``**`` with the usual meaning: ``*`` never crosses a ``/``,
``**`` does.
"""
from __future__ import annotations

import re
from functools import lru_cache
from typing import Iterable, Sequence


@lru_cache(maxsize=2048)
def _compiled(pattern: str) -> re.Pattern:
    return re.compile(_translate(pattern))


def _translate(pattern: str) -> str:
    out = ["^"]
    i, n = 0, len(pattern)
    while i < n:
        c = pattern[i]
        if c == "*":
            if pattern[i:i + 3] == "**/":
                out.append("(?:.*/)?")
                i += 3
                continue
            if pattern[i:i + 2] == "**":
                out.append(".*")
                i += 2
                continue
            out.append("[^/]*")
            i += 1
            continue
        if c == "?":
            out.append("[^/]")
            i += 1
            continue
        if c == "[":
            j = pattern.find("]", i + 1)
            if j < 0:
                out.append(re.escape(c))
                i += 1
                continue
            body = pattern[i + 1:j]
            if body.startswith("!"):
                body = "^" + body[1:]
            out.append("[" + body + "]")
            i = j + 1
            continue
        out.append(re.escape(c))
        i += 1
    out.append("$")
    return "".join(out)


def matches(pattern: str, path: str) -> bool:
    """True if *path* (repo-relative, '/'-separated) matches *pattern*.

    A pattern with no wildcard also matches everything beneath it, so
    ``build`` covers ``build/CMakeCache.txt`` without needing ``build/**``.
    """
    # NB: not lstrip("./") — that strips characters, mangling dotted paths
    # such as ".harness/state.json" into "harness/state.json".
    if path.startswith("./"):
        path = path[2:]
    pattern = pattern.strip()
    if not pattern:
        return False
    if _compiled(pattern).match(path):
        return True
    if not any(ch in pattern for ch in "*?["):
        prefix = pattern.rstrip("/") + "/"
        return path.startswith(prefix)
    return False


def matches_any(patterns: Iterable[str], path: str) -> bool:
    return any(matches(p, path) for p in patterns)


def first_match(patterns: Sequence[str], path: str) -> str | None:
    for p in patterns:
        if matches(p, path):
            return p
    return None
