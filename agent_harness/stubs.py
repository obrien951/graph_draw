"""The unfilled-call protocol.

Walking the graph foot-to-leaf means a node is implemented *before* the things
it calls exist.  Rather than let each agent invent its own placeholder, the
harness fixes one marker:

    HARNESS-STUB(<exact node name>)

An agent implementing node N declares whatever N needs from an unimplemented
dependency D — the header, the signature, the type — and leaves the body
unfilled, tagged ``HARNESS-STUB(D)``.  When the walk later reaches D, the
harness greps the tree for that tag and hands its agent every call site, so D
gets implemented against real usage instead of a guess.

The marker is deliberately language-neutral: it lives in a comment, so it
works in C++, CMake, Python or Rust alike.
"""
from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

from .pathglob import matches_any

MARKER = "HARNESS-STUB"

#: A node's own escape hatch: "I could not finish this, here is why" rather
#: than a forward-declared dependency. See find_partial_reason() below.
PARTIAL_MARKER = "HARNESS-PARTIAL"

#: The harness's own sources quote the marker in docs and examples; never scan
#: them, or every run reports its own documentation as an unfilled call.
HARNESS_OWN_PATHS = ("agent_harness/**", "graph_agent.py", "harness.scope.json",
                     "tests/test_agent_harness.py")


def marker_for(name: str) -> str:
    return f"{MARKER}({name})"


def partial_marker_for(name: str) -> str:
    return f"{PARTIAL_MARKER}({name})"


def _files_containing(root: Path, needle: str, ignore: Sequence[str]) -> list[Path]:
    """Every file (tracked or not) that contains *needle*, ignore-list applied."""
    try:
        out = subprocess.run(
            ["git", "grep", "-l", "-F", needle], cwd=str(root),
            capture_output=True, text=True,
        )
        names = [l for l in out.stdout.splitlines() if l.strip()]
    except OSError:
        names = []
    # git grep misses untracked files; sweep them too.
    try:
        extra = subprocess.run(
            ["git", "ls-files", "--others", "--exclude-standard"], cwd=str(root),
            capture_output=True, text=True,
        ).stdout.splitlines()
    except OSError:
        extra = []
    seen: list[Path] = []
    for name in dict.fromkeys(names + extra):
        if matches_any(ignore, name):
            continue
        p = root / name
        if p.is_file():
            seen.append(p)
    return seen


def find_partial_reason(root: str | Path, name: str, ignore: Sequence[str] = ()) -> str | None:
    """The text after a HARNESS-PARTIAL(<name>) marker still in the tree, if any.

    None means the node carries no such marker — either it was never left
    partial, or a later pass (a double-check round, a human) already removed
    it by finishing the work. A marker found with no reason text after it
    still counts as present, just with a placeholder explanation.
    """
    root = Path(root)
    needle = partial_marker_for(name)
    pattern = re.compile(re.escape(needle) + r"\s*:?\s*(.*)")
    for path in _files_containing(root, needle, tuple(ignore) + HARNESS_OWN_PATHS):
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for line in text.splitlines():
            if needle not in line:
                continue
            m = pattern.search(line)
            reason = m.group(1).strip().rstrip("*/").strip() if m else ""
            return reason or "(no reason given)"
    return None


#: Per-language body an agent should leave behind.  Shown in the prompt.
STUB_EXAMPLES = {
    "c++": (
        "// HARNESS-STUB(ir::Repo): declared here, implemented by its own node.\n"
        "ir::Repo RustAnalyzer::analyze(const RepoFileIndex& index) const {\n"
        "    Q_UNIMPLEMENTED();   // or: throw std::logic_error(\"HARNESS-STUB(...)\");\n"
        "    return {};\n"
        "}"
    ),
    "python": (
        "def merge(curated, generated):\n"
        "    # HARNESS-STUB(GraphMerger::merge): implemented by its own node.\n"
        "    raise NotImplementedError(\"HARNESS-STUB(GraphMerger::merge)\")"
    ),
    "rust": (
        "// HARNESS-STUB(scan_rust_file): implemented by its own node.\n"
        "pub fn scan_rust_file(text: &str) -> RustFileItems {\n"
        "    unimplemented!(\"HARNESS-STUB(scan_rust_file)\")\n"
        "}"
    ),
}


@dataclass(frozen=True)
class StubSite:
    path: str
    line: int
    excerpt: str

    def render(self) -> str:
        return f"{self.path}:{self.line}\n{self.excerpt}"


class StubIndex:
    """Finds outstanding HARNESS-STUB markers in the working tree."""

    def __init__(self, root: str | Path, ignore: Sequence[str] = (), context_lines: int = 6):
        self.root = Path(root)
        self.ignore = tuple(ignore) + HARNESS_OWN_PATHS
        self.context_lines = context_lines

    def _candidate_files(self) -> list[Path]:
        return _files_containing(self.root, MARKER, self.ignore)

    def sites_for(self, name: str, limit: int = 12) -> list[StubSite]:
        needle = marker_for(name)
        found: list[StubSite] = []
        for path in self._candidate_files():
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            if needle not in text:
                continue
            lines = text.splitlines()
            for i, line in enumerate(lines):
                if needle not in line:
                    continue
                lo = max(0, i - self.context_lines)
                hi = min(len(lines), i + self.context_lines + 1)
                excerpt = "\n".join(
                    f"{'>' if j == i else ' '} {j + 1:5d} | {lines[j]}" for j in range(lo, hi)
                )
                found.append(StubSite(str(path.relative_to(self.root)), i + 1, excerpt))
                if len(found) >= limit:
                    return found
        return found

    def outstanding(self) -> dict[str, list[StubSite]]:
        """Every marker still present, keyed by the node name it names."""
        pattern = re.compile(re.escape(MARKER) + r"\(([^)]+)\)")
        out: dict[str, list[StubSite]] = {}
        for path in self._candidate_files():
            try:
                lines = path.read_text(encoding="utf-8").splitlines()
            except (OSError, UnicodeDecodeError):
                continue
            for i, line in enumerate(lines):
                for m in pattern.finditer(line):
                    out.setdefault(m.group(1).strip(), []).append(
                        StubSite(str(path.relative_to(self.root)), i + 1, line.strip())
                    )
        return out


def unresolved_report(index: StubIndex, expected_done: Iterable[str]) -> list[str]:
    """Names that should have been filled in by now but still carry a marker."""
    outstanding = index.outstanding()
    return sorted(name for name in expected_done if name in outstanding)
