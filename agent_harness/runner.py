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
from .gate import REVIEW, VERIFY, Gate
from .graphmodel import KIND_ARTIFACT, Graph, Node, ROOT_FIRST
from .prompts import PromptBuilder, parse_noop_verdict
from .references import ReferenceResource, place_artifact
from .review import Reviewer
from .scope import ScopeAuditor, ScopeResolver, Violation
from .state import DONE, FAILED, NOOP, PARTIAL, RUNNING, SKIPPED, RunState
from .stubs import PARTIAL_MARKER, StubIndex, find_partial_reason
from .verify import Verifier
from .workspace import GitWorkspace

ON_VIOLATION_REVERT = "revert"
ON_VIOLATION_RETRY = "revert-retry"
ON_VIOLATION_WARN = "warn"
ON_VIOLATION_FAIL = "fail"
VIOLATION_POLICIES = (ON_VIOLATION_RETRY, ON_VIOLATION_REVERT, ON_VIOLATION_WARN, ON_VIOLATION_FAIL)

# Named grounds for a failed attempt: every place _attempt_loop can bail out
# on a node, so state.json and the run summary say WHY it stayed
# unimplemented instead of just THAT it did. VERIFY and REVIEW are the gate's
# own stage names (gate.py) reused here rather than duplicated.
REASON_AGENT_ERROR = "agent-error"          # the backend itself did not complete: crash,
                                             # timeout, or an HTTP/context-window error
REASON_SCOPE_VIOLATION = "scope-violation"  # the agent edited outside its allowed fence
REASON_NO_CHANGES = "no-changes"            # the agent's turn ended with no file changes
REASON_VERIFY_FAILED = VERIFY               # the build/test gate did not pass
REASON_REVIEW_FAILED = REVIEW               # the review agent requested changes
REASON_NO_ATTEMPT = "no-attempt"            # attempts exhausted with no attempt recorded
REASON_PARTIAL_DISALLOWED = "partial-disallowed"  # left a HARNESS-PARTIAL marker but
                                                   # --allow-partial is off for this run


class SafetyNetLost(RuntimeError):
    """An agent turn deleted the harness's own state directory — most often
    `rm -rf .harness/` during a --fix-rounds turn, having misread "untracked
    in git status" as "safe to delete" (str_tsne_rs, 2026-08-30: a fix round
    for the clustering node ran `rm -rf .harness/ src/clustering/` because
    both showed up as untracked). .harness/** is deliberately invisible to
    the scope auditor so the harness's own writes are never flagged as
    violations — which also means nothing catches the agent deleting it
    directly. Once that happens, no revert for any node can be trusted, so
    the whole walk stops here rather than silently continuing on a
    compromised safety net.
    """

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
    double_check_rounds: int = 0
    allow_partial: bool = False
    noop_check: bool = False
    confirm_changes: bool = False
    fix_rounds: int = 0
    #: run each Artifact node's `search` through a search engine before its
    #: brief is built — seed the candidate URLs into the brief and, when one
    #: clearly matches, place the file at its path with a provenance sidecar.
    fetch_refs: bool = False
    search_url: str | None = None


@dataclass
class NodeOutcome:
    node: Node
    status: str
    attempts: int = 0
    changed: dict[str, str] = field(default_factory=dict)
    violations: list[Violation] = field(default_factory=list)
    note: str = ""
    reason: str = ""
    seconds: float = 0.0
    verified: bool = False
    reviewed: bool = False
    double_checked: int = 0


def _tail_of(text: str, limit: int = 400) -> str:
    """The tail of an agentic backend's own transcript, for when it ran to
    completion but wrote nothing — usually the one place it explains why."""
    flat = " ".join(text.split()) if text else ""
    if not flat:
        return ""
    return flat if len(flat) <= limit else "…" + flat[-limit:]


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

    def _gate_diff(self, node: Node, snapshot) -> str:
        """The diff handed to the gate. An Artifact node's diff is a whole
        downloaded data file — megabytes of no use to a reviewer, and the gate
        skips review for it anyway — so don't even generate it."""
        if not self.workspace or node.kind == KIND_ARTIFACT:
            return ""
        return self.workspace.diff(snapshot)

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
        outcome: NodeOutcome | None = None
        try:
            if self.options.noop_check:
                outcome = self._check_noop(node, entry, started)
                if outcome is not None:
                    return outcome
            outcome = self._attempt_loop(node, entry, started)
            return outcome
        finally:
            # A clean FAILED return already means "revert" (below). But a
            # KeyboardInterrupt or a crash mid-attempt — GPU contention killing
            # the opencode subprocess, say — unwinds straight past that return
            # and would otherwise leave whatever partial edits were on disk at
            # the moment of interruption sitting there, unverified, forever.
            # That is exactly how an interrupted double-check round left
            # graph_merge/graphmerger.cpp half-rewritten and non-compiling on
            # 2026-08-25: the process died before this line ever ran.
            failed = outcome is None or outcome.status == FAILED
            if failed and self.options.revert_on_failure:
                self._rewind(node, entry)

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

    def _check_noop(self, node: Node, entry, started: float) -> NodeOutcome | None:
        """A cheap, read-only pre-check: does the codebase already satisfy this
        node, making a full write-capable attempt pure waste? Returns a
        terminal NOOP outcome if the agent can point to exactly where the job
        is already done, else None so the normal attempt loop proceeds.

        Fails closed at every step: an agent that can't run, that answers
        ambiguously, that writes something despite being told not to, or
        whose NOOP claim doesn't survive an actual verify run, all fall
        through to a real implementation attempt rather than risk a false
        "nothing to do" silently leaving a HARNESS-STUB unfilled forever.
        """
        agent = self._backend_for(node)
        if not self.workspace or not agent.agentic:
            return None
        scope = self.scopes.resolve(node)
        prompt = self.prompts.noop_check_prompt(node, scope)
        log_path = self.state_dir / "logs" / f"{node.slug}.noopcheck.log"
        request = RunRequest(prompt=prompt, cwd=self.root,
                             label=f"no-op check {node.kind} {node.name}",
                             log_path=log_path, writes_files=False)
        request.prompt += agent.prompt_suffix(request)
        self.log(f"  no-op check (scope: {', '.join(scope.allow) or 'unset'})")
        result = agent.run(request)
        if not agent.mutates:
            return None
        if not result.ok:
            self.log(f"  no-op check failed to run: {result.error[:200]} — implementing instead")
            return None

        stray = self.workspace.changes(entry)
        if stray:
            self.workspace.restore(entry, sorted(stray))
            self.log(f"  no-op check wrote {len(stray)} file(s) despite being told not to; "
                     f"reverted — implementing instead")
            return None

        is_noop, evidence = parse_noop_verdict(result.text)
        if not is_noop:
            self.log("  no-op check: real work remains")
            return None

        verdict = self.verifier.run(self.state_dir / "logs" / f"{node.slug}.noopcheck.verify.log") \
            if self.verifier.enabled() else None
        if verdict is not None and not verdict.ok:
            self.log(f"  no-op check claimed NOOP ({evidence}) but the build/test gate "
                     f"currently fails — not trusting the claim, implementing instead")
            return None

        self.log(f"  no-op: {evidence}")
        if self.options.update_graph:
            self.graph.mark_implemented(node.index)
            if self.graph.path:
                self.graph.save()
        return NodeOutcome(node, NOOP, 0, {}, note=evidence,
                           verified=verdict.ok if verdict is not None else False,
                           seconds=time.time() - started)

    def _confirm_changes(self, node: Node, agent: Backend, attempt: int) -> None:
        """A blunt, cheap follow-up in the SAME opencode session: did that
        turn actually write anything, or did it stop at planning?

        This is deliberately not --double-check: double-check is a fresh
        session re-reading the spec against the diff, expensive and thorough.
        This is one line, in the same conversation the model just had, giving
        it one more chance to notice it talked about writing code without
        ever calling a tool to do it — exactly the failure mode a reasoning
        model stopping mid-plan produces (see rustlex::blank, 2026-08-28).
        Best-effort: a failure here does not fail the attempt: the original
        result already stands on its own merits either way.
        """
        log_path = self.state_dir / "logs" / f"{node.slug}.attempt{attempt}.confirm.log"
        request = RunRequest(
            prompt="Were the changes made? Make sure you actually did what was asked.",
            cwd=self.root, label=f"confirm {node.kind} {node.name}",
            log_path=log_path, continue_session=True,
        )
        request.prompt += agent.prompt_suffix(request)
        self.log("  confirming the changes were actually made")
        result = agent.run(request)
        if not result.ok:
            self.log(f"  confirm follow-up failed to run: {result.error[:200]}")

    def _fix_round(self, node: Node, agent: Backend, attempt: int, round_no: int,
                   problem: str, tag: str) -> bool:
        """One fixer turn in the SAME session as the attempt that was just
        rejected — a scope violation or a failed gate (build/test or
        review) — handing it the specific reason and asking it to fix
        exactly that instead of the harness reverting the whole attempt and
        starting over from a blank context. Bounded by --fix-rounds at each
        call site, so a rejection that never resolves still terminates.

        Returns whether the round itself ran (result.ok) — not whether the
        underlying problem is actually fixed; the caller re-checks that.
        """
        log_path = self.state_dir / "logs" / f"{node.slug}.attempt{attempt}.fix{round_no}.{tag}.log"
        request = RunRequest(
            prompt=self.prompts.fix_prompt(node, problem),
            cwd=self.root, label=f"fix {node.kind} {node.name}",
            log_path=log_path, continue_session=True,
        )
        request.prompt += agent.prompt_suffix(request)
        self.log(f"  fix round {round_no}/{self.options.fix_rounds} ({tag})")
        result = agent.run(request)
        if not result.ok:
            self.log(f"  fix round {round_no} failed to run: {result.error[:200]}")
        if not self.state_dir.exists():
            self.log(f"  CRITICAL: {self.state_dir} no longer exists after fix round "
                     f"{round_no} on {node.name} — the agent deleted the harness's own "
                     f"working directory. No further revert can be trusted; stopping.")
            raise SafetyNetLost(
                f"{self.state_dir} was deleted during a fix round on {node.name} "
                f"(attempt {attempt}, round {round_no})"
            )
        return result.ok

    def _attempt_loop(self, node: Node, entry, started: float) -> NodeOutcome:
        scope = self.scopes.resolve(node)
        feedback = ""
        last: NodeOutcome | None = None
        # Computed once, from the tree as it stood before this call's first
        # attempt: a marker already present here came from an EARLIER partial
        # acceptance (this run or a prior one), not from an attempt we are
        # about to make. Every attempt's brief gets told about it.
        existing_partial = self._partial_marker(node) if self.workspace else None
        if existing_partial:
            self.log(f"  resuming partial work: {existing_partial}")

        for attempt in range(1, self.options.attempts + 1):
            self.log(f"  attempt {attempt}/{self.options.attempts} "
                     f"(scope: {', '.join(scope.allow) or 'unset'})")
            work_dir = self.state_dir / "work" / node.slug
            work_dir.mkdir(parents=True, exist_ok=True)
            log_path = self.state_dir / "logs" / f"{node.slug}.attempt{attempt}.log"

            snapshot = entry
            brief = self._brief(node, scope, feedback, existing_partial)
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

            if result.ok and self.options.confirm_changes and agent.supports_continue:
                self._confirm_changes(node, agent, attempt)

            changed = self.workspace.changes(snapshot) if self.workspace else {}
            added = self.workspace.added_line_count(snapshot) if self.workspace else 0

            if not result.ok:
                detail = f"agent failed (exit {result.exit_code}): {result.error[:400]}"
                self.log(f"  attempt {attempt} failed [{REASON_AGENT_ERROR}]: {detail}")
                last = NodeOutcome(node, FAILED, attempt, changed,
                                   reason=REASON_AGENT_ERROR, note=detail,
                                   seconds=time.time() - started)
                feedback = f"The previous attempt failed to run: {result.error[:1500]}"
                continue

            violations, offenders = self._audit(scope, changed, added)
            if violations and self.options.fix_rounds and agent.supports_continue:
                for round_no in range(1, self.options.fix_rounds + 1):
                    self.log(f"  attempt {attempt}: {len(violations)} scope violation(s), "
                             f"trying a fix instead of reverting:")
                    for v in violations:
                        self.log(f"    {v}")
                    self._fix_round(node, agent, attempt, round_no,
                                    self._violation_feedback(violations), "scope")
                    changed = self.workspace.changes(snapshot) if self.workspace else {}
                    added = self.workspace.added_line_count(snapshot) if self.workspace else 0
                    violations, offenders = self._audit(scope, changed, added)
                    if not violations:
                        self.log(f"  fix round {round_no}: scope violation resolved")
                        break
                else:
                    self.log(f"  scope violation not resolved after "
                             f"{self.options.fix_rounds} fix round(s)")
            if violations:
                self.log(f"  attempt {attempt} failed [{REASON_SCOPE_VIOLATION}]: "
                         f"{len(violations)} violation(s)")
                for v in violations:
                    self.log(f"    {v}")
                handled = self._handle_violations(snapshot, offenders, violations)
                if handled is not None:
                    outcome = NodeOutcome(node, handled, attempt, changed, violations,
                                          reason=REASON_SCOPE_VIOLATION,
                                          note="; ".join(str(v) for v in violations)[:500],
                                          seconds=time.time() - started)
                    if handled == FAILED and self.options.on_violation == ON_VIOLATION_RETRY:
                        feedback = self._violation_feedback(violations)
                        last = outcome
                        continue
                    return outcome
                changed = self.workspace.changes(snapshot) if self.workspace else {}

            if not changed:
                tail = _tail_of(result.text)
                detail = "the agent changed nothing" + (f" — its own summary: {tail}" if tail else "")
                self.log(f"  attempt {attempt} failed [{REASON_NO_CHANGES}]: {detail}")
                last = NodeOutcome(node, FAILED, attempt, changed,
                                   reason=REASON_NO_CHANGES, note=detail,
                                   seconds=time.time() - started)
                feedback = ("The previous attempt produced no file changes at all. "
                            "Implement the node by editing files inside the allowed paths.")
                continue

            # The gate: build/test commands, then the review agent, in that
            # order, dispatched here by code rather than at an agent's option.
            verdict = self.gate().check(
                node, scope, self._gate_diff(node, snapshot),
                verify_log=self.state_dir / "logs" / f"{node.slug}.verify.log",
                review_log=self.state_dir / "logs" / f"{node.slug}.review{attempt}.log",
            )
            if not verdict.ok and self.options.fix_rounds and agent.supports_continue:
                for round_no in range(1, self.options.fix_rounds + 1):
                    self.log(f"  attempt {attempt} failed [{verdict.stage}], trying a fix "
                             f"instead of restarting: {verdict.note}")
                    self._fix_round(node, agent, attempt, round_no, verdict.feedback(), "gate")
                    changed = self.workspace.changes(snapshot) if self.workspace else {}
                    added = self.workspace.added_line_count(snapshot) if self.workspace else 0
                    # A fix round can itself stray out of scope; catch that
                    # the same way a normal attempt would rather than let an
                    # unaudited write through just because it happened here.
                    fix_violations, fix_offenders = self._audit(scope, changed, added)
                    if fix_violations:
                        self.log(f"  fix round {round_no}: {len(fix_violations)} scope "
                                 f"violation(s), reverting")
                        self.workspace.restore(snapshot, fix_offenders) if self.workspace else None
                        changed = self.workspace.changes(snapshot) if self.workspace else {}
                        continue
                    verdict = self.gate().check(
                        node, scope, self._gate_diff(node, snapshot),
                        verify_log=self.state_dir / "logs" / f"{node.slug}.verify.log",
                        review_log=self.state_dir / "logs" / f"{node.slug}.review{attempt}.fix{round_no}.log",
                    )
                    if verdict.ok:
                        self.log(f"  fix round {round_no}: gate now passes")
                        break
                else:
                    self.log(f"  {verdict.stage} still failing after "
                             f"{self.options.fix_rounds} fix round(s)")

            if not verdict.ok:
                self.log(f"  attempt {attempt} failed [{verdict.stage}]: {verdict.note}")
                feedback = verdict.feedback()
                last = NodeOutcome(node, FAILED, attempt, changed, note=verdict.note,
                                   reason=verdict.stage,
                                   verified=verdict.verify_ran, reviewed=verdict.review_ran,
                                   seconds=time.time() - started)
                continue

            partial_reason = self._partial_marker(node)
            if partial_reason and not self.options.allow_partial:
                detail = (f"left a {PARTIAL_MARKER} marker but --allow-partial is off: "
                          f"{partial_reason}")
                self.log(f"  attempt {attempt} failed [{REASON_PARTIAL_DISALLOWED}]: {detail}")
                feedback = (
                    f"You left a HARNESS-PARTIAL({node.name}) marker: \"{partial_reason}\". "
                    "Partial completion is not enabled for this run — finish the node fully, "
                    "or if it is genuinely infeasible, say so plainly in your summary instead "
                    "of leaving the marker."
                )
                last = NodeOutcome(node, FAILED, attempt, changed, note=detail,
                                   reason=REASON_PARTIAL_DISALLOWED,
                                   verified=verdict.verify_ran, reviewed=verdict.review_ran,
                                   seconds=time.time() - started)
                continue

            changed, dc_rounds = self._double_check(node, scope, entry, changed)
            if self.workspace:
                added = self.workspace.added_line_count(entry)

            # Double-check may have finished what was left partial — re-check
            # rather than trust the pre-double-check verdict.
            partial_reason = self._partial_marker(node) if self.options.allow_partial else None
            if partial_reason:
                self.log(f"  partial: kept incomplete — {partial_reason}")
                return NodeOutcome(node, PARTIAL, attempt, changed, note=partial_reason,
                                   verified=verdict.verify_ran, reviewed=verdict.review_ran,
                                   double_checked=dc_rounds,
                                   seconds=time.time() - started)

            self._finish(node, changed)
            self.log(f"  done: {len(changed)} file(s), +{added} lines")
            return NodeOutcome(node, DONE, attempt, changed,
                               verified=verdict.verify_ran, reviewed=verdict.review_ran,
                               double_checked=dc_rounds,
                               seconds=time.time() - started)

        return last or NodeOutcome(node, FAILED, self.options.attempts,
                                   reason=REASON_NO_ATTEMPT,
                                   note="no attempt succeeded", seconds=time.time() - started)

    def _partial_marker(self, node: Node) -> str | None:
        if not self.workspace:
            return None
        return find_partial_reason(self.root, node.name, self.scopes.config.ignore)

    def _double_check(self, node: Node, scope, entry, changed: dict[str, str]
                      ) -> tuple[dict[str, str], int]:
        """After a node's implementation already passed the gate once, ask the
        same agent to re-read its own diff against the node's specification
        and fix anything that drifted, up to ``double_check_rounds`` times.

        Each round is snapshotted, audited and re-verified on its own: one
        that breaks the fence or the gate is reverted back to the last good
        state rather than trading a working node for a broken one, and a
        round that changes nothing ends the loop early. Only meaningful for
        an agentic backend that can read the tree itself; a completion-only
        backend has nothing to double-check against.
        """
        rounds = self.options.double_check_rounds
        agent = self._backend_for(node)
        if not rounds or not self.workspace or not agent.agentic:
            return changed, 0

        prompt = self.prompts.double_check_prompt(node, scope, self.verifier.commands)
        ran = 0
        for round_no in range(1, rounds + 1):
            self.log(f"  double-check {round_no}/{rounds}")
            ran += 1
            good = self.workspace.snapshot(self.state_dir / "backup" / f"{node.slug}.dc{round_no}")
            log_path = self.state_dir / "logs" / f"{node.slug}.doublecheck{round_no}.log"
            request = RunRequest(prompt=prompt, cwd=self.root,
                                 label=f"double-check {node.kind} {node.name}", log_path=log_path)
            request.prompt += agent.prompt_suffix(request)
            result = agent.run(request)
            if not agent.mutates:
                continue
            if not result.ok:
                self.log(f"  double-check {round_no} failed [{REASON_AGENT_ERROR}] "
                         f"(exit {result.exit_code}): {result.error[:200]}")
                # An agentic backend writes to disk as it goes, so a timeout or crash
                # mid-round can still leave partial edits behind. Those were never
                # scope-audited or gate-checked, so they are not trustworthy — revert
                # them rather than silently folding them into a "done" node.
                stray = self.workspace.changes(good)
                if stray:
                    self.workspace.restore(good, sorted(stray))
                    self.log(f"  double-check {round_no}: reverted {len(stray)} file(s) "
                             f"left by the failed round")
                break

            round_changed = self.workspace.changes(good)
            if not round_changed:
                self.log(f"  double-check {round_no}: no discrepancies found")
                continue

            round_added = self.workspace.added_line_count(good)
            violations, _ = self._audit(scope, round_changed, round_added)
            if violations:
                self.log(f"  double-check {round_no} failed [{REASON_SCOPE_VIOLATION}]: "
                         f"{len(violations)} violation(s), reverting")
                self.workspace.restore(good, sorted(round_changed))
                continue

            verdict = self.gate().check(
                node, scope, self._gate_diff(node, good),
                verify_log=self.state_dir / "logs" / f"{node.slug}.doublecheck{round_no}.verify.log",
                review_log=self.state_dir / "logs" / f"{node.slug}.doublecheck{round_no}.review.log",
            )
            if not verdict.ok:
                self.log(f"  double-check {round_no} failed [{verdict.stage}]: "
                         f"{verdict.note}, reverting")
                self.workspace.restore(good, sorted(round_changed))
                continue

            self.log(f"  double-check {round_no}: rectified {len(round_changed)} file(s)")

        return self.workspace.changes(entry), ran

    # ----------------------------------------------------------- the pieces
    def _snapshot(self, node: Node):
        if not self.workspace:
            return None
        return self.workspace.snapshot(self.state_dir / "backup" / node.slug)

    def _brief(self, node: Node, scope, feedback: str, existing_partial: str | None = None):
        sections = self.context.build(
            self.graph, node, scope.allow,
            extra_files=self._existing(self.options.extra_context),
            plan_path=self.options.plan_path,
            dir_of=lambda n: self.scopes.resolve(n).module_dir,
        )
        artifact_resource = None
        if node.kind == KIND_ARTIFACT and self.options.fetch_refs:
            artifact_resource = self._retrieve_artifact(node)
        return self.prompts.build(
            node, scope, sections,
            stub_sites=self.stubs.sites_for(node.name),
            verify_cmds=self.verifier.commands,
            feedback=feedback,
            order_note=ORDER_NOTES.get(self.options.order, ""),
            allow_partial=self.options.allow_partial,
            existing_partial=existing_partial,
            artifact_resource=artifact_resource,
        )

    def _retrieve_artifact(self, node: Node) -> ReferenceResource:
        """Search for the artifact and, when a result clearly matches, place it
        at its path with a provenance sidecar — so the node's agent turn is a
        verification, not a from-scratch download. Fails open: on no result the
        resource just carries the query + candidates for the brief."""
        res = ReferenceResource.from_node(node)
        try:
            return place_artifact(res, self.root, self.state_dir / "refs",
                                  search_url=self.options.search_url, log=self.log)
        except Exception as exc:  # noqa: BLE001 — retrieval must never break the walk
            self.log(f"  artifact retrieval errored ({exc}); leaving it to the agent")
            return res

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
        ns.reason = outcome.reason
        ns.verified = outcome.verified
        ns.reviewed = outcome.reviewed
        ns.double_checked = outcome.double_checked
        ns.seconds = round(outcome.seconds, 1)
        ns.finished_at = time.strftime("%Y-%m-%d %H:%M:%S")
        self.state.save()

    # -------------------------------------------------------------- reports
    def outstanding_stubs(self) -> dict[str, list]:
        return self.stubs.outstanding()
