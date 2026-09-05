"""The scope fence: what one node's agent is allowed to touch.

Scope creep is prevented in four layers, and this module owns three of them:

1. **Declared** — every node resolves to an explicit allow-list of paths,
   derived from the Module that contains it in the graph plus whatever the
   scope config adds.  Nothing is inferred at run time by the agent.
2. **Stated**   — the allow-list, the deny-list and the budgets are printed
   into the prompt verbatim (see prompts.py), so the fence is not a surprise.
3. **Enforced** — after the agent exits, the real diff is compared against the
   allow-list and the budgets; violations are reverted (see workspace.py).

The fourth layer is the review pass (review.py), which reads the diff and
objects to work that belongs to another node even when it landed in a legal
file.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

from .graphmodel import KIND_ARTIFACT, KIND_MODULE, Graph, Node
from .pathglob import first_match, matches_any

DEFAULT_IGNORE = (
    ".git/**", "build/**", "cmake-build-*/**", "**/CMakeFiles/**", ".idea/**", ".qt/**",
    "**/*.o", "**/*.a", "**/*.so", "**/*.dylib", "**/.DS_Store", "**/Makefile",
    "**/cmake_install.cmake", "**/CTestTestfile.cmake", "**/compile_commands.json",
    ".harness/**", "**/__pycache__/**",
)

DEFAULT_PROTECTED = (
    ".git/**", ".harness/**", "agent_harness/**", "graph_agent.py",
)

DEFAULT_MAX_FILES = 10
DEFAULT_MAX_ADDED_LINES = 900


@dataclass(frozen=True)
class Scope:
    node: str
    allow: tuple[str, ...]
    deny: tuple[str, ...]
    max_files: int
    max_added_lines: int
    module: str | None = None
    module_dir: str | None = None
    notes: tuple[str, ...] = ()

    def permits(self, path: str) -> bool:
        if matches_any(self.deny, path):
            return False
        return matches_any(self.allow, path)

    def blocking_deny(self) -> str | None:
        """The deny pattern that would swallow this node's real work, or None.

        Deny always wins over allow (see permits/ScopeAuditor.audit). The
        node's own directory is where its actual implementation lands; shared
        glue paths (CMakeLists.txt, shared test fixtures) can't substitute for
        it, so that directory is checked directly rather than requiring the
        whole allow-list to be dead — a node can still "have" writable paths
        in the glue files while its real work is blocked. --freeze is applied
        globally for a run, so this most often happens when a directory was
        frozen as "already finished" and the graph later gained a new,
        unbuilt node inside it.
        """
        if not self.allow:
            return "(no allow-list)"
        if self.module_dir:
            return first_match(self.deny, f"{self.module_dir}/__probe__")
        culprit: str | None = None
        for pattern in self.allow:
            probe = pattern.replace("**", "__probe__")
            hit = first_match(self.deny, probe)
            if hit is None:
                return None
            culprit = hit
        return culprit

    def describe(self) -> str:
        lines = [f"ALLOWED PATHS (create or modify only these):"]
        lines += [f"  + {p}" for p in self.allow]
        if self.deny:
            lines.append("FORBIDDEN PATHS (never touch, even if it looks helpful):")
            lines += [f"  - {p}" for p in self.deny]
        lines.append(f"BUDGET: at most {self.max_files} changed files and "
                     f"{self.max_added_lines} added lines for this node.")
        lines += [f"NOTE: {n}" for n in self.notes]
        return "\n".join(lines)


@dataclass
class ScopeConfig:
    """Declarative scope policy, loaded from JSON (see harness.scope.json)."""

    allow: tuple[str, ...] = ("{module_dir}/**",)
    shared_allow: tuple[str, ...] = ()
    deny: tuple[str, ...] = DEFAULT_PROTECTED
    ignore: tuple[str, ...] = DEFAULT_IGNORE
    max_files: int = DEFAULT_MAX_FILES
    max_added_lines: int = DEFAULT_MAX_ADDED_LINES
    modules: dict = field(default_factory=dict)
    nodes: dict = field(default_factory=dict)
    context_files: tuple[str, ...] = ()
    verify: tuple[str, ...] = ()
    #: "rust" | "c++" | "python" | ...  — overridden by --language, and only
    #: consulted when neither --language nor repo autodetection settled it.
    language: str | None = None
    #: published datasets a node's spec depends on; see references.py
    reference_resources: tuple[dict, ...] = ()

    @classmethod
    def load(cls, path: str | Path | None) -> "ScopeConfig":
        if path is None:
            return cls()
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        base = cls()
        defaults = data.get("defaults", {})
        return cls(
            allow=tuple(defaults.get("allow", base.allow)),
            shared_allow=tuple(data.get("shared_allow", base.shared_allow)),
            deny=tuple(data.get("protected", base.deny)) + DEFAULT_PROTECTED,
            # Always union with DEFAULT_IGNORE, never replace it outright — a
            # custom "ignore" list omitting .harness/** (as str_tsne_rs's did)
            # makes the harness's own log/work/brief writes look like agent
            # changes, which the deny list then flags as forbidden-path
            # violations and reverts, discarding real work for no reason.
            # deny already gets this guarantee via "+ DEFAULT_PROTECTED"
            # above; ignore did not, which is exactly the bug that happened.
            ignore=tuple(dict.fromkeys(list(data.get("ignore", ())) + list(DEFAULT_IGNORE))),
            max_files=int(defaults.get("max_files", base.max_files)),
            max_added_lines=int(defaults.get("max_added_lines", base.max_added_lines)),
            modules=dict(data.get("modules", {})),
            nodes=dict(data.get("nodes", {})),
            context_files=tuple(data.get("context_files", ())),
            verify=tuple(data.get("verify", ())),
            language=data.get("language"),
            reference_resources=tuple(data.get("reference_resources", ())),
        )


class ScopeResolver:
    """Turns (graph, node) into the Scope its agent must stay inside."""

    def __init__(self, graph: Graph, config: ScopeConfig | None = None):
        self.graph = graph
        self.config = config or ScopeConfig()

    def module_dir(self, module_name: str | None) -> str | None:
        if not module_name:
            return None
        entry = self.config.modules.get(module_name, {})
        return entry.get("dir", module_name)

    def resolve(self, node: Node) -> Scope:
        cfg = self.config
        module = node if node.kind == KIND_MODULE else self.graph.owner_module(node.index)
        module_name = module.name if module else None
        mdir = self.module_dir(module_name)
        mentry = cfg.modules.get(module_name or "", {})
        nentry = cfg.nodes.get(node.name, {})

        patterns: list[str] = []
        patterns += list(nentry.get("allow", []))
        patterns += list(mentry.get("allow", cfg.allow))
        patterns += list(mentry.get("shared_allow", cfg.shared_allow))
        patterns += list(nentry.get("allow_extra", []))

        allow: list[str] = []
        for p in patterns:
            if "{module_dir}" in p:
                if not mdir:
                    continue
                p = p.replace("{module_dir}", mdir)
            p = p.replace("{module}", module_name or "").replace("{node_slug}", node.slug)
            if p not in allow:
                allow.append(p)

        max_files = int(nentry.get("max_files", mentry.get("max_files", cfg.max_files)))
        max_added = int(nentry.get("max_added_lines",
                                   mentry.get("max_added_lines", cfg.max_added_lines)))
        artifact_note = None
        if node.kind == KIND_ARTIFACT:
            # An Artifact node writes one downloaded file plus its provenance
            # sidecar, nowhere else. The file's own path (from the graph JSON)
            # is the fence; a published data file also blows any sane
            # added-lines budget, so that check is lifted for this kind.
            art_path = str(node.raw.get("path", "")).strip()
            if art_path:
                for extra in (art_path, f"{art_path}.provenance.json"):
                    if extra not in allow:
                        allow.append(extra)
            max_files = int(nentry.get("max_files", 4))
            max_added = max(max_added, 50_000_000)
            artifact_note = ("This is an Artifact node: download the real file, place it "
                             "at the path above, write its .provenance.json, touch nothing else.")

        deny = list(dict.fromkeys(list(nentry.get("deny", [])) + list(cfg.deny)))
        notes = tuple(filter(None, [nentry.get("note"), mentry.get("note"), artifact_note]))
        return Scope(
            node=node.name,
            allow=tuple(allow),
            deny=tuple(deny),
            max_files=max_files,
            max_added_lines=max_added,
            module=module_name,
            module_dir=mdir,
            notes=notes,
        )


@dataclass
class Violation:
    kind: str          # out-of-scope | forbidden | budget-files | budget-lines
    detail: str

    def __str__(self) -> str:
        return f"[{self.kind}] {self.detail}"


class ScopeAuditor:
    """Checks a set of realised changes against a Scope."""

    def __init__(self, scope: Scope):
        self.scope = scope

    def audit(self, changes: dict[str, str], added_lines: int) -> tuple[list[Violation], list[str]]:
        """Return (violations, offending paths to revert)."""
        violations: list[Violation] = []
        offenders: list[str] = []
        for path, status in changes.items():
            if matches_any(self.scope.deny, path):
                violations.append(Violation("forbidden", f"{status} {path} (protected path)"))
                offenders.append(path)
            elif not matches_any(self.scope.allow, path):
                violations.append(Violation("out-of-scope", f"{status} {path} (not in the allow-list)"))
                offenders.append(path)
        in_scope = [p for p in changes if p not in set(offenders)]
        if len(in_scope) > self.scope.max_files:
            violations.append(Violation(
                "budget-files",
                f"{len(in_scope)} files changed, budget is {self.scope.max_files}: {', '.join(sorted(in_scope))}",
            ))
        if added_lines > self.scope.max_added_lines:
            violations.append(Violation(
                "budget-lines",
                f"{added_lines} lines added, budget is {self.scope.max_added_lines}",
            ))
        return violations, offenders


def missing_work(changes: dict[str, str]) -> bool:
    return not changes
