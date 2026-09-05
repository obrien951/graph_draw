"""The completion gate: everything that must pass before a node counts as done.

This lives apart from the runner because the *order* and the *fail-closed
policy* are the point, not an implementation detail of the walk.

The harness is a Python program precisely so that the checks are dispatched by
code rather than left to an agent's discretion. So the gate is composed here,
called from exactly one place in the traversal, and cannot be skipped by a model
deciding its work looks fine:

    build/test commands  ->  review agent  ->  node may be marked implemented

Cheap and deterministic first, expensive and probabilistic second: there is no
point paying a review model to read a diff that does not compile.

The 2026-08-19 run marked 39 of 43 nodes implemented on a tree that did not
link, because the gate was empty and the reviewer was never dispatched. Every
default here is chosen so that a missing signal reads as failure rather than
success.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .graphmodel import KIND_ARTIFACT, Node
from .references import ReferenceResource, check_artifact
from .review import Reviewer
from .scope import Scope
from .stubs import find_partial_reason
from .verify import Verifier

VERIFY, REVIEW, ARTIFACT = "verify", "review", "artifact"


@dataclass
class GateResult:
    """Why a node was let through, or what stopped it."""

    ok: bool = True
    stage: str = ""          # VERIFY or REVIEW when ok is False
    note: str = ""           # short, for the run summary and state.json
    detail: str = ""         # the agent-facing feedback for the retry
    verify_ran: bool = False
    review_ran: bool = False

    def feedback(self) -> str:
        return self.detail


class Gate:
    """Runs the build, then the reviewer, and reports the first refusal.

    `require_review` turns the reviewer from advisory into binding: a reviewer
    that is not configured, cannot be reached, or answers unintelligibly fails
    the node instead of silently passing it. Off by default because it makes a
    flaky endpoint stop the walk; worth turning on for an unattended run.
    """

    def __init__(self, verifier: Verifier, reviewer: Reviewer, require_review: bool = False):
        self.verifier = verifier
        self.reviewer = reviewer
        self.require_review = require_review

    # ------------------------------------------------------------------ info
    def describe(self) -> str:
        bits = []
        bits.append(f"{len(self.verifier.commands)} command(s)"
                    if self.verifier.enabled() else "no build gate")
        if self.reviewer.enabled():
            bits.append("review required" if self.require_review else "review advisory")
        else:
            bits.append("review required but NOT CONFIGURED"
                        if self.require_review else "no review")
        return " + ".join(bits)

    def misconfigured(self) -> str:
        """A configuration that can never pass, reported before the walk starts."""
        if self.require_review and not self.reviewer.enabled():
            return ("--require-review was given but no review backend is configured; "
                    "every node would fail. Pass --review-backend, or drop --require-review.")
        return ""

    # ----------------------------------------------------------- the baseline
    def baseline(self, log_path: Path | None = None) -> GateResult:
        """Verify the tree *before* the walk.

        If the repository is already broken, every node fails for a reason that
        has nothing to do with the node, and the logs are unreadable. Better to
        say so once, up front.
        """
        if not self.verifier.enabled():
            return GateResult(ok=True)
        verdict = self.verifier.run(log_path)
        if verdict.ok:
            return GateResult(ok=True, verify_ran=True)
        return GateResult(ok=False, stage=VERIFY, verify_ran=True,
                          note=f"baseline verify failed: {verdict.command}",
                          detail=verdict.feedback())

    # -------------------------------------------------------------- per node
    def check(self, node: Node, scope: Scope, diff: str,
              verify_log: Path | None = None,
              review_log: Path | None = None) -> GateResult:
        """The whole gate for one node's attempt, in order, stopping at the first no."""
        result = GateResult(ok=True)

        if self.verifier.enabled():
            verdict = self.verifier.run(verify_log)
            result.verify_ran = True
            if not verdict.ok:
                return GateResult(ok=False, stage=VERIFY, verify_ran=True,
                                  note=f"verify failed: {verdict.command}",
                                  detail=verdict.feedback())

        if node.kind == KIND_ARTIFACT:
            # A downloaded data file has nothing for a code reviewer to judge;
            # the deterministic check (file present, non-empty, sha256) stands
            # in for the review stage and is fail-closed like the rest of the
            # gate. A declared HARNESS-PARTIAL defers to the runner, same as a
            # code node.
            root = self.reviewer.root
            if find_partial_reason(root, node.name):
                return result
            check = check_artifact(ReferenceResource.from_node(node), root)
            if not check.ok:
                return GateResult(ok=False, stage=ARTIFACT, verify_ran=result.verify_ran,
                                  note=check.note, detail=check.detail)
            result.note = check.note
            return result

        if not self.reviewer.enabled():
            if self.require_review:
                return GateResult(ok=False, stage=REVIEW, verify_ran=result.verify_ran,
                                  note="review required but no reviewer is configured",
                                  detail="No review backend is configured for this run.")
            return result

        outcome = self.reviewer.review(node, scope, diff, log_path=review_log)
        result.review_ran = outcome.ran

        if not outcome.ran:
            # review.py fails open here, returning passed=True when the reviewer
            # could not be reached or the diff was empty. Under --require-review
            # that silence is treated as a refusal instead.
            if self.require_review and diff.strip():
                return GateResult(ok=False, stage=REVIEW, verify_ran=result.verify_ran,
                                  note="reviewer did not run",
                                  detail="The review agent could not be reached, and this run "
                                         "requires a review before a node may be marked done.")
            return result

        if not outcome.passed:
            return GateResult(ok=False, stage=REVIEW, verify_ran=result.verify_ran,
                              review_ran=True,
                              note=f"review requested changes ({len(outcome.findings)} finding(s))",
                              detail=outcome.feedback())
        return result
