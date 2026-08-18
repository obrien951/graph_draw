"""File-block protocol for backends that cannot edit files themselves.

An agent CLI (opencode) writes to the tree directly.  A plain completion model
served by llama.cpp cannot, so it answers with blocks and the harness writes
them.  One strict format, no fuzzy fence sniffing:

    <<<FILE path/to/file>>>
    ...whole new contents of the file...
    <<<END>>>

    <<<DELETE path/to/file>>>

Paths are repo-relative; absolute paths and ``..`` are refused here, and the
scope fence checks them again after the write.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

FILE_RE = re.compile(r"^<<<FILE\s+(?P<path>[^>]+)>>>\s*$(?P<body>.*?)^<<<END>>>\s*$",
                     re.M | re.S)
DELETE_RE = re.compile(r"^<<<DELETE\s+(?P<path>[^>]+)>>>\s*$", re.M)

PROTOCOL_HELP = """\
## Output protocol — you cannot edit files directly, so answer with blocks
Return the COMPLETE new contents of every file you create or change, like this:

<<<FILE relative/path/to/file.cpp>>>
...entire file contents, not a patch, not an excerpt...
<<<END>>>

Use one block per file, repo-relative paths only. To remove a file:

<<<DELETE relative/path.cpp>>>

Write a short plain-text summary before the first block. Anything outside a
block is treated as commentary and ignored.
"""


@dataclass(frozen=True)
class FileBlock:
    path: str
    body: str | None          # None means delete

    @property
    def is_delete(self) -> bool:
        return self.body is None


class PatchError(RuntimeError):
    pass


def parse_blocks(text: str) -> list[FileBlock]:
    blocks: list[FileBlock] = []
    for m in FILE_RE.finditer(text):
        body = m.group("body")
        if body.startswith("\n"):
            body = body[1:]
        blocks.append(FileBlock(m.group("path").strip(), body))
    for m in DELETE_RE.finditer(text):
        path = m.group("path").strip()
        if not any(b.path == path for b in blocks):
            blocks.append(FileBlock(path, None))
    return blocks


def _safe_relative(root: Path, path: str) -> Path:
    candidate = Path(path)
    if candidate.is_absolute() or ".." in candidate.parts:
        raise PatchError(f"refusing unsafe path {path!r}")
    resolved = (root / candidate).resolve()
    if not str(resolved).startswith(str(root.resolve())):
        raise PatchError(f"refusing path outside the repo: {path!r}")
    return resolved


class FileBlockApplier:
    """Writes parsed blocks into the working tree."""

    def __init__(self, root: str | Path):
        self.root = Path(root).resolve()

    def apply(self, blocks: Sequence[FileBlock]) -> tuple[list[str], list[str]]:
        """Return (written paths, errors)."""
        written: list[str] = []
        errors: list[str] = []
        for block in blocks:
            try:
                target = _safe_relative(self.root, block.path)
            except PatchError as exc:
                errors.append(str(exc))
                continue
            try:
                if block.is_delete:
                    if target.exists():
                        target.unlink()
                        written.append(block.path)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                body = block.body or ""
                if body and not body.endswith("\n"):
                    body += "\n"
                target.write_text(body, encoding="utf-8")
                written.append(block.path)
            except OSError as exc:
                errors.append(f"{block.path}: {exc}")
        return written, errors
