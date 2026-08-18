"""Packing the existing codebase into the prompt.

An agent that cannot see what already exists writes a second copy of it, so
context selection *is* the DRY mechanism.  Four sections, each capped, in
increasing order of cost:

* the file tree              — where things live
* the symbol index           — what already exists, so it gets reused
* selected file bodies       — the node's own module, its implemented
                               dependencies' headers, plus anything the caller
                               passed with --context-file/--context-glob
* the plan excerpt           — the section of IMPLEMENTATION_PLAN.md whose task
                               id the node's comment cites (B1, A7, C0 ...)
"""
from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

from .graphmodel import Graph, Node
from .pathglob import matches_any

SOURCE_SUFFIXES = (
    ".h", ".hpp", ".hh", ".c", ".cc", ".cpp", ".cxx", ".rs", ".py", ".txt", ".cmake",
    ".json", ".md", ".toml", ".ts", ".js",
)

SYMBOL_PATTERNS = (
    re.compile(r"^\s*(?:class|struct)\s+([A-Za-z_]\w*)", re.M),
    re.compile(r"^\s*namespace\s+([A-Za-z_]\w*)", re.M),
    re.compile(r"^\s*(?:pub\s+)?(?:fn|trait|enum|impl)\s+([A-Za-z_]\w*)", re.M),
    re.compile(r"^\s*def\s+([A-Za-z_]\w*)", re.M),
    re.compile(r"^\s*add_(?:library|executable)\s*\(\s*([A-Za-z_][\w-]*)", re.M),
)

TASK_ID = re.compile(r"\b([ABC]\d{1,2})\b")


@dataclass
class Budget:
    total_chars: int = 90_000
    tree_chars: int = 6_000
    symbols_chars: int = 10_000
    plan_chars: int = 14_000
    file_chars: int = 24_000        # per file
    max_files: int = 14


@dataclass
class Section:
    title: str
    body: str

    def render(self) -> str:
        return f"### {self.title}\n{self.body.rstrip()}\n"


class RepoContext:
    """Reads the working tree and assembles per-node context sections."""

    def __init__(self, root: str | Path, ignore: Sequence[str] = (), budget: Budget | None = None):
        self.root = Path(root).resolve()
        self.ignore = tuple(ignore)
        self.budget = budget or Budget()
        self._files: list[str] | None = None

    # ------------------------------------------------------------ file lists
    def files(self) -> list[str]:
        if self._files is None:
            listed: list[str] = []
            for args in (["git", "ls-files"], ["git", "ls-files", "--others", "--exclude-standard"]):
                try:
                    listed += subprocess.run(
                        args, cwd=str(self.root), capture_output=True, text=True
                    ).stdout.splitlines()
                except OSError:
                    pass
            self._files = sorted({
                f for f in listed
                if f and not matches_any(self.ignore, f) and (self.root / f).is_file()
            })
        return self._files

    def matching(self, patterns: Iterable[str]) -> list[str]:
        patterns = list(patterns)
        if not patterns:
            return []
        return [f for f in self.files() if matches_any(patterns, f)]

    def read(self, rel: str, limit: int | None = None) -> str | None:
        try:
            text = (self.root / rel).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            return None
        cap = limit or self.budget.file_chars
        if len(text) > cap:
            text = text[:cap] + f"\n... [{rel} truncated at {cap} chars]\n"
        return text

    # -------------------------------------------------------------- sections
    def tree_section(self) -> Section:
        files = [f for f in self.files() if Path(f).suffix in SOURCE_SUFFIXES or "/" not in f]
        body = "\n".join(files)
        if len(body) > self.budget.tree_chars:
            body = body[:self.budget.tree_chars] + "\n... [tree truncated]"
        return Section("Repository layout (tracked source files)", body)

    def symbol_section(self) -> Section:
        lines: list[str] = []
        for rel in self.files():
            if Path(rel).suffix not in SOURCE_SUFFIXES:
                continue
            text = self.read(rel, limit=200_000)
            if not text:
                continue
            names: list[str] = []
            for pat in SYMBOL_PATTERNS:
                for m in pat.finditer(text):
                    if m.group(1) not in names:
                        names.append(m.group(1))
            if names:
                lines.append(f"{rel}: {', '.join(names[:24])}")
        body = "\n".join(lines)
        if len(body) > self.budget.symbols_chars:
            body = body[:self.budget.symbols_chars] + "\n... [symbol index truncated]"
        return Section(
            "Existing symbols — REUSE these, do not reimplement them", body or "(none found)"
        )

    def files_section(self, paths: Sequence[str], title: str) -> Section | None:
        chunks: list[str] = []
        for rel in list(dict.fromkeys(paths))[: self.budget.max_files]:
            body = self.read(rel)
            if body is None:
                continue
            chunks.append(f"--- {rel} ---\n{body}")
        if not chunks:
            return None
        return Section(title, "\n\n".join(chunks))

    def plan_section(self, plan_path: str | Path | None, node: Node) -> Section | None:
        if not plan_path:
            return None
        path = Path(plan_path)
        if not path.is_absolute():
            path = self.root / path
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            return None
        ids = list(dict.fromkeys(TASK_ID.findall(node.comment)))
        if not ids:
            return None
        blocks: list[str] = []
        for task in ids:
            block = self._task_block(text, task)
            if block:
                blocks.append(block)
        if not blocks:
            return None
        body = "\n\n".join(blocks)
        if len(body) > self.budget.plan_chars:
            body = body[:self.budget.plan_chars] + "\n... [plan excerpt truncated]"
        return Section(
            f"Plan excerpt for task(s) {', '.join(ids)} (from {path.name}) — authoritative detail",
            body,
        )

    @staticmethod
    def _task_block(text: str, task: str) -> str | None:
        """Grab the paragraph/heading block that introduces a task id."""
        start = re.search(rf"^(#+\s*|\*\*){re.escape(task)}\b.*$", text, re.M)
        if not start:
            start = re.search(rf"^.*\b{re.escape(task)}\b\s*[—-].*$", text, re.M)
        if not start:
            return None
        begin = start.start()
        nxt = re.search(r"^(#+\s+|\*\*[ABC]\d{1,2}\b)", text[start.end():], re.M)
        end = start.end() + (nxt.start() if nxt else 4000)
        return text[begin:end].strip()

    # ------------------------------------------------------------- assembling
    def node_paths(self, graph: Graph, node: Node, allow: Sequence[str]) -> list[str]:
        """Existing files worth showing: the node's own scope, then its
        implemented dependencies' and dependents' modules."""
        picked = self.matching(allow)
        related: list[str] = []
        for other in graph.dependencies(node.index) + graph.dependents(node.index):
            if not other.implemented:
                continue
            module = other if other.kind == "Module" else graph.owner_module(other.index)
            if module is None:
                continue
            related += [
                f for f in self.matching([f"{module.name}/**"])
                if Path(f).suffix in (".h", ".hpp", ".cpp", ".txt", ".rs", ".py")
            ]
        ordered = list(dict.fromkeys(picked + related))
        return ordered[: self.budget.max_files]

    def build(
        self,
        graph: Graph,
        node: Node,
        allow: Sequence[str],
        extra_files: Sequence[str] = (),
        plan_path: str | Path | None = None,
    ) -> list[Section]:
        sections: list[Section] = [self.tree_section(), self.symbol_section()]
        plan = self.plan_section(plan_path, node)
        if plan:
            sections.append(plan)
        forced = self.files_section(list(extra_files), "Caller-supplied context files")
        if forced:
            sections.append(forced)
        near = self.files_section(
            [p for p in self.node_paths(graph, node, allow) if p not in set(extra_files)],
            "Code near this node (its module and its implemented neighbours)",
        )
        if near:
            sections.append(near)
        return self._fit(sections)

    def _fit(self, sections: list[Section]) -> list[Section]:
        out, used = [], 0
        for s in sections:
            rendered = s.render()
            if used + len(rendered) > self.budget.total_chars:
                room = max(0, self.budget.total_chars - used)
                if room > 500:
                    out.append(Section(s.title, s.body[: room - 200] + "\n... [truncated to fit budget]"))
                break
            out.append(s)
            used += len(rendered)
        return out
