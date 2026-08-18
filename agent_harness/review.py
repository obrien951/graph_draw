"""The DRY/SOLID/scope review pass.

A second model reads only the diff the node produced and answers PASS or
REVISE with findings.  Findings are fed straight back into the implementing
agent's retry, which is why review.py depends on prompts.py for the wording
and on nothing else: it is a policy, not a mechanism.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from .backends import Backend, RunRequest
from .graphmodel import Node
from .prompts import PromptBuilder, parse_review
from .scope import Scope


@dataclass
class ReviewOutcome:
    passed: bool = True
    findings: list[str] = field(default_factory=list)
    raw: str = ""
    ran: bool = False

    def feedback(self) -> str:
        bullets = "\n".join(f"  - {f}" for f in self.findings)
        return ("A reviewer rejected the previous attempt on scope/DRY/SOLID grounds. "
                "Fix exactly these points and nothing else:\n" + bullets)


class Reviewer:
    """Optional quality gate between a node's run and marking it done."""

    def __init__(self, backend: Backend | None, builder: PromptBuilder, root: Path):
        self.backend = backend
        self.builder = builder
        self.root = root

    def enabled(self) -> bool:
        return self.backend is not None

    def review(self, node: Node, scope: Scope, diff: str, log_path: Path | None = None) -> ReviewOutcome:
        if self.backend is None or not diff.strip():
            return ReviewOutcome()
        prompt = self.builder.review_prompt(node, scope, diff)
        result = self.backend.run(RunRequest(
            prompt=prompt, cwd=self.root, label=f"review {node.name}",
            log_path=log_path, writes_files=False,
        ))
        if not result.ok:
            # A reviewer that cannot run must not block the build.
            return ReviewOutcome(passed=True, raw=result.error, ran=False)
        passed, findings = parse_review(result.text)
        return ReviewOutcome(passed=passed or not findings, findings=findings,
                             raw=result.text, ran=True)
