"""The walk itself.

For every selected node, in graph order:

    snapshot the tree -> build the brief -> run the agent -> audit the diff
    against the fence -> revert what escaped -> verify the build -> review for
    DRY/SOLID/scope -> retry with feedback, or mark the node implemented.

The runner owns the sequencing and nothing else; each step lives in its own
module and is injected here, so a different backend, fence or reviewer changes
no code in this file.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Sequence

from .backends import Backend, RunRequest
from .context import RepoContext
from .gate import Gate
from .graphmodel import Graph, Node, ROOT_FIRST
from .prompts import PromptBuilder
from .review import Reviewer
from .scope import ScopeAuditor, ScopeResolver, Violation
from .state import DONE, FAILED, RUNNING, SKIPPED, RunState
from .stubs import StubIndex
from .verify import Verifier
from .workspace import GitWorkspace

ON_VIOLATION_REVERT = "revert"
ON_VIOLATION_RETRY = "revert-retry"
ON_VIOLATION_WARN = "warn"
ON_VIOLATION_FAIL = "fail"
VIOLATION_POLICIES = (ON_VIOLATION_RETRY, ON_VIOLATION_REVERT, ON_VIOLATION_WARN, ON_VIOLATION_FAIL)

ORDER_NOTES = {
    ROOT_FIRST: ("The walk goes foot-to-leaf: callers are implemented before the things they "
                 "call, so the pieces below you do not exist yet and you must leave unfilled "
                 "calls for them."),
}


@dataclass
class HarnessOptions:
    order: str = ROOT_FIRST
    attempts: int = 2
    on_violation: str = ON_VIOLATION_RETRY
    inline_limit: int = 60_000
    update_graph: bool = True
    commit: bool = False
    commit_template: str = "harness: implement {kind} {name}"
    pause: bool = False
    plan_path: str | None = None
    extra_context: tuple[str, ...] = ()
    stop_on_failure: bool = False
    revert_on_failure: bool = True
    require_review: bool = False


@dataclass
class NodeOutcome:
    node: Node
    status: str
    attempts: int = 0
    changed: dict[str, str] = field(default_factory=dict)
    violations: list[Violation] = field(default_factory=list)
    note: str = ""
    seconds: float = 0.0
    verified: bool = False
    reviewed: bool = False


class Harness:
    """Drives one graph to completion (or to the first thing that goes wrong)."""

    def __init__(
        self,
        graph: Graph,
        root: Path,
        backend: Backend,
        scopes: ScopeResolver,
        context: RepoContext,
        prompts: PromptBuilder,
        workspace: GitWorkspace | None,
        verifier: Verifier,
        reviewer: Reviewer,
        state: RunState,
        state_dir: Path,
        options: HarnessOptions,
        log: Callable[[str], None] = print,
        strong_backend: Backend | None = None,
        strong_nodes: Sequence[str] = (),
        strong_kinds: Sequence[str] = (),
    ):
        self.graph = graph
        self.root = root
        self.backend = backend
        self.scopes = scopes
        self.context = context
        self.prompts = prompts
        self.workspace = workspace
        self.verifier = verifier
        self.reviewer = reviewer
        self.state = state
        self.state_dir = state_dir
        self.options = options
        self.log = log
        self.strong_backend = strong_backend
        self.strong_nodes = frozenset(strong_nodes)
        self.strong_kinds = frozenset(strong_kinds)
        self.stubs = StubIndex(root, ignore=scopes.config.ignore)

    def gate(self) -> Gate:
        """The completion gate, rebuilt per call so late-swapped parts are honoured."""
        return Gate(self.verifier, self.reviewer, self.options.require_review)

    # ------------------------------------------------------------------ walk
    def run(self, indices: Sequence[int]) -> list[NodeOutcome]:
        outcomes: list[NodeOutcome] = []
        total = len(indices)

        # Verify once before touching anything. A tree that is already broken
        # makes every node fail for reasons that are not the node's fault.
        baseline = self.gate().baseline(self.state_dir / "logs" / "baseline.verify.log")
        if not baseline.ok:
            self.log(f"  {baseline.note}")
            self.log("  refusing to start: fix the tree first, or drop the verify commands")
            return outcomes

        for position, index in enumerate(indices, start=1):
            node = self.graph.node(index)
            self.log(f"\n[{position}/{total}] {node.kind} {node.name}")
            if self.options.pause and not self._confirm(node):
                outcomes.append(NodeOutcome(node, SKIPPED, note="skipped by operator"))
                self._record(outcomes[-1])
                continue
            outcome = self.run_node(node)
            outcomes.append(outcome)
            self._record(outcome)
            if outcome.status == FAILED and self.options.stop_on_failure:
                self.log("  stopping: --stop-on-failure and this node failed")
                break
        return outcomes

    def _confirm(self, node: Node) -> bool:
        try:
            answer = input(f"  run {node.name}? [Y/n/q] ").strip().lower()
        except EOFError:
            return True
        if answer in ("q", "quit"):
            raise KeyboardInterrupt
        return answer in ("", "y", "yes")

    # ------------------------------------------------------------- one node
    def _backend_for(self, node: Node) -> Backend:
        """Structural nodes may warrant a stronger (slower, costlier) model.

        The 2026-08-19 run showed a small model can write a plausible-looking
        interface it cannot then implement, so the shared contracts are worth
        paying more for while leaf functions stay cheap.
        """
        if self.strong_backend and (node.name in self.strong_nodes
                                    or node.kind in self.strong_kinds):
            return self.strong_backend
        return self.backend

    def run_node(self, node: Node) -> NodeOutcome:
        """Build one node, leaving the tree untouched if it does not succeed."""
        started = time.time()
        node_state = self.state.get(node.name, node.kind)
        node_state.status = RUNNING
        self.state.save()

        # One snapshot for the whole node, not one per attempt: it is both the
        # diff base and the point we rewind to if every attempt fails. Taking it
        # per attempt would overwrite the backup we need to restore from.
        entry = self._snapshot(node)
        outcome = self._attempt_loop(node, entry, started)
        if outcome.status == FAILED and self.options.revert_on_failure:
            self._rewind(node, entry)
        return outcome

    def _rewind(self, node: Node, entry) -> None:
        """Undo everything a failed node wrote.

        Without this a failed node leaves half-written files behind that the
        next node then sees, treats as real, and builds on.
        """
        if not (self.workspace and entry):
            return
        stray = sorted(self.workspace.changes(entry))
        if not stray:
            return
        restored = self.workspace.restore(entry, stray)
        self.log(f"  reverted {len(restored)} file(s) from the failed node")

    def _attempt_loop(self, node: Node, entry, started: float) -> NodeOutcome:
        scope = self.scopes.resolve(node)
        feedback = ""
        last: NodeOutcome | None = None

        for attempt in range(1, self.options.attempts + 1):
            self.log(f"  attempt {attempt}/{self.options.attempts} "
                     f"(scope: {', '.join(scope.allow) or 'unset'})")
            work_dir = self.state_dir / "work" / node.slug
            work_dir.mkdir(parents=True, exist_ok=True)
            log_path = self.state_dir / "logs" / f"{node.slug}.attempt{attempt}.log"

            snapshot = entry
            brief = self._brief(node, scope, feedback)
            prompt = self._deliver(brief, work_dir)
            (work_dir / f"brief.attempt{attempt}.md").write_text(brief.full(), encoding="utf-8")

            request = RunRequest(prompt=prompt, cwd=self.root,
                                 label=f"{node.kind} {node.name}", log_path=log_path)
            request.prompt += self._backend_for(node).prompt_suffix(request)
            agent = self._backend_for(node)
            result = agent.run(request)
            if not agent.mutates:
                self.log(f"  brief written to {work_dir}/brief.attempt{attempt}.md "
                         f"({len(prompt)} chars); no agent launched")
                return NodeOutcome(node, SKIPPED, attempt, note="dry run",
                                   seconds=time.time() - started)

            changed = self.workspace.changes(snapshot) if self.workspace else {}
            added = self.workspace.added_line_count(snapshot) if self.workspace else 0

            if not result.ok:
                last = NodeOutcome(node, FAILED, attempt, changed,
                                   note=f"agent failed: {result.error[:400]}",
                                   seconds=time.time() - started)
                feedback = f"The previous attempt failed to run: {result.error[:1500]}"
                continue

            violations, offenders = self._audit(scope, changed, added)
            if violations:
                self.log(f"  scope: {len(violations)} violation(s)")
                for v in violations:
                    self.log(f"    {v}")
                handled = self._handle_violations(snapshot, offenders, violations)
                if handled is not None:
                    outcome = NodeOutcome(node, handled, attempt, changed, violations,
                                          note="; ".join(str(v) for v in violations)[:500],
                                          seconds=time.time() - started)
                    if handled == FAILED and self.options.on_violation == ON_VIOLATION_RETRY:
                        feedback = self._violation_feedback(violations)
                        last = outcome
                        continue
                    return outcome
                changed = self.workspace.changes(snapshot) if self.workspace else {}

            if not changed:
                last = NodeOutcome(node, FAILED, attempt, changed,
                                   note="the agent changed nothing",
                                   seconds=time.time() - started)
                feedback = ("The previous attempt produced no file changes at all. "
                            "Implement the node by editing files inside the allowed paths.")
                continue

            # The gate: build/test commands, then the review agent, in that
            # order, dispatched here by code rather than at an agent's option.
            verdict = self.gate().check(
                node, scope,
                self.workspace.diff(snapshot) if self.workspace else "",
                verify_log=self.state_dir / "logs" / f"{node.slug}.verify.log",
                review_log=self.state_dir / "logs" / f"{node.slug}.review{attempt}.log",
            )
            if not verdict.ok:
                self.log(f"  {verdict.stage}: {verdict.note}")
                feedback = verdict.feedback()
                last = NodeOutcome(node, FAILED, attempt, changed, note=verdict.note,
                                   verified=verdict.verify_ran, reviewed=verdict.review_ran,
                                   seconds=time.time() - started)
                continue

            self._finish(node, changed)
            self.log(f"  done: {len(changed)} file(s), +{added} lines")
            return NodeOutcome(node, DONE, attempt, changed,
                               verified=verdict.verify_ran, reviewed=verdict.review_ran,
                               seconds=time.time() - started)

        return last or NodeOutcome(node, FAILED, self.options.attempts,
                                   note="no attempt succeeded", seconds=time.time() - started)

    # ----------------------------------------------------------- the pieces
    def _snapshot(self, node: Node):
        if not self.workspace:
            return None
        return self.workspace.snapshot(self.state_dir / "backup" / node.slug)

    def _brief(self, node: Node, scope, feedback: str):
        sections = self.context.build(
            self.graph, node, scope.allow,
            extra_files=self._existing(self.options.extra_context),
            plan_path=self.options.plan_path,
        )
        return self.prompts.build(
            node, scope, sections,
            stub_sites=self.stubs.sites_for(node.name),
            verify_cmds=self.verifier.commands,
            feedback=feedback,
            order_note=ORDER_NOTES.get(self.options.order, ""),
        )

    def _existing(self, paths: Sequence[str]) -> list[str]:
        return [p for p in paths if (self.root / p).is_file()]

    def _deliver(self, brief, work_dir: Path) -> str:
        if len(brief.full()) <= self.options.inline_limit:
            return brief.full()
        context_file = work_dir / "context.md"
        context_file.write_text(brief.context, encoding="utf-8")
        try:
            shown = str(context_file.relative_to(self.root))
        except ValueError:
            shown = str(context_file)
        return brief.delivered(shown)

    def _audit(self, scope, changed: dict[str, str], added: int):
        if not self.workspace:
            return [], []
        return ScopeAuditor(scope).audit(changed, added)

    def _handle_violations(self, snapshot, offenders, violations) -> str | None:
        """Returns a terminal status, or None to carry on with this attempt."""
        policy = self.options.on_violation
        if policy == ON_VIOLATION_WARN:
            return None
        if policy == ON_VIOLATION_FAIL:
            return FAILED
        # revert and revert-retry both undo whatever escaped the fence; they
        # differ only in whether the node is then retried with feedback.
        if offenders and self.workspace:
            restored = self.workspace.restore(snapshot, offenders)
            self.log(f"  reverted {len(restored)} out-of-scope path(s)")
        return FAILED if policy == ON_VIOLATION_RETRY else None

    @staticmethod
    def _violation_feedback(violations) -> str:
        lines = "\n".join(f"  - {v}" for v in violations)
        return ("The previous attempt broke the scope fence and those changes were reverted.\n"
                f"{lines}\n"
                "Redo the node using only the allowed paths and stay inside the budget. If the "
                "node truly cannot be built that way, stop and explain instead of editing.")

    def _finish(self, node: Node, changed: dict[str, str]) -> None:
        if self.options.update_graph:
            self.graph.mark_implemented(node.index)
            if self.graph.path:
                self.graph.save()
        if self.options.commit and self.workspace:
            message = self.options.commit_template.format(name=node.name, kind=node.kind)
            sha = self.workspace.commit(message, list(changed))
            if sha:
                self.log(f"  committed {sha[:8]}")

    def _record(self, outcome: NodeOutcome) -> None:
        ns = self.state.get(outcome.node.name, outcome.node.kind)
        ns.status = outcome.status
        ns.attempts = outcome.attempts
        ns.changed = sorted(outcome.changed)
        ns.violations = [str(v) for v in outcome.violations]
        ns.note = outcome.note
        ns.verified = outcome.verified
        ns.reviewed = outcome.reviewed
        ns.seconds = round(outcome.seconds, 1)
        ns.finished_at = time.strftime("%Y-%m-%d %H:%M:%S")
        self.state.save()

    # -------------------------------------------------------------- reports
    def outstanding_stubs(self) -> dict[str, list]:
        return self.stubs.outstanding()
