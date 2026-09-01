"""Symbol indexing backend for RepoContext.symbol_section().

The harness has always rebuilt the symbol map fresh before every node's
prompt (RepoContext.symbol_section() re-scans the tree on every call, so
it reflects whatever the previous node just wrote) — that part isn't new.
What was weak was the *source* of that map: a handful of top-level regexes
that catch a class/fn/struct declaration but not its members, and carry no
line numbers, so an agent still has to open a file and read it to find the
actual method it's meant to reuse.

This prefers `ctags` (specifically universal-ctags — the classic BSD/Emacs
ctags shipped by Xcode's command line tools accepts none of the flags used
here) for real per-language parsing, including members and line numbers.
When it isn't on PATH, `build_index` returns None and the caller keeps
using its own regex fallback — this module never raises on a missing or
wrong ctags, since the harness has to keep working on a box that doesn't
have universal-ctags installed.
"""
from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path
from typing import Sequence

_CTAGS_LANGUAGE_SUFFIXES = {
    ".h", ".hpp", ".hh", ".c", ".cc", ".cpp", ".cxx", ".rs", ".py",
}


def _is_universal_ctags(binary: str) -> bool:
    try:
        proc = subprocess.run(
            [binary, "--version"], capture_output=True, text=True, timeout=5
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return "Universal Ctags" in proc.stdout


def find_ctags() -> str | None:
    """The first universal-ctags binary on PATH, or None.

    Checked by running --version rather than trusting the name: on macOS
    `ctags` on PATH is routinely the Xcode-bundled BSD ctags, which parses
    a different, incompatible flag set and would otherwise fail silently
    or (worse) hang waiting on stdin.
    """
    seen: set[str] = set()
    for name in ("ctags-universal", "universal-ctags", "ctags"):
        path = shutil.which(name)
        if not path or path in seen:
            continue
        seen.add(path)
        if _is_universal_ctags(path):
            return path
    return None


def build_index(root: Path, files: Sequence[str], *, binary: str | None = None) -> str | None:
    """Run ctags over `files` (paths relative to `root`) and render a
    per-file, line-numbered symbol listing. Returns None if ctags isn't
    available, none of the files are a language it parses, or the call
    fails or times out for any reason — always a fallback, never a crash.
    """
    binary = binary or find_ctags()
    if binary is None:
        return None
    targets = [f for f in files if Path(f).suffix in _CTAGS_LANGUAGE_SUFFIXES]
    if not targets:
        return None
    try:
        proc = subprocess.run(
            [binary, "-R", "--output-format=json", "--fields=+n", "-f", "-", *targets],
            cwd=str(root), capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    by_file: dict[str, list[tuple[int, str]]] = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            tag = json.loads(line)
        except json.JSONDecodeError:
            continue
        if tag.get("_type") != "tag":
            continue
        path, name, kind = tag.get("path"), tag.get("name"), tag.get("kind")
        if not path or not name:
            continue
        lineno = tag.get("line", 0)
        scope = tag.get("scope")
        label = f"{scope}::{name}" if scope else name
        by_file.setdefault(path, []).append((lineno, f"  {lineno}\t{kind or '?'}\t{label}"))
    if not by_file:
        return None
    out: list[str] = []
    for path in sorted(by_file):
        out.append(f"{path}:")
        out.extend(text for _, text in sorted(by_file[path]))
    return "\n".join(out)
