"""Prompt construction.

One node, one agent, one brief.  The brief is assembled from named templates
so that each rule has exactly one home: the mission, the dependency contract
(the unfilled-call protocol), the scope fence, the DRY/SOLID rules, the
context pack, and the definition of done.

The brief splits into a ``core`` (never dropped, always inline) and a
``context`` (large, spillable to a file the agent is told to read), so the same
text serves a tool-using agent and a plain completion model.
"""
from __future__ import annotations

import textwrap
from dataclasses import dataclass
from typing import Sequence

from .context import Section
from .graphmodel import Graph, Node
from .scope import Scope
from .stubs import STUB_EXAMPLES, StubSite, marker_for

MISSION = """\
# Implement one graph node: {name}

You are one agent in a graph-driven build. The dependency graph of this project
is the plan: every node is a Module, Class or Function, and an edge `A -> B`
means A depends on B. You have been given exactly one node. Another agent gets
each of the others.{order_note}

## The node
kind          : {kind}
name          : {name}
owning module : {module} -> {module_dir}

## Its description — this is the specification, follow it literally
{description}
"""

DEPENDENCIES = """\
## Dependencies that already exist — call them, never re-create them
{existing}

## Dependencies that DO NOT exist yet — leave unfilled calls
These nodes are scheduled after yours. You must NOT implement them. Declare
exactly what you need from each (header, signature, type), write the call as if
it worked, and leave the body unfilled with the marker `{marker_shape}` so the
agent that owns it can find your call site:
{missing}

Shape of an unfilled call:
```
{stub_example}
```
A stub must still compile and link. A signature you invent here is a contract
the next agent inherits: keep it minimal and exactly as the dependency's own
description implies.

## Nodes that depend on you — design the interface they need, and no more
{callers}
"""

STUB_SITES = """\
## Unfilled calls waiting on YOU — fill these in place
An earlier agent already wrote calls against this node and left the body
unfilled. Implement against these real call sites, keep the signatures they
committed to unless they are wrong (say so if they are), and delete the
`{marker}` markers as you fill them.

{sites}
"""

FENCE = """\
## Scope fence — the hardest rule here
{fence}

Anything you change outside the allowed paths is reverted automatically once you
exit, and this node is retried from scratch. That is a wasted run, so:
  - do not implement, refactor or "tidy" another node's code;
  - do not touch a test that is named as a regression contract;
  - do not add dependencies, build targets or files the description never asks for;
  - if the node genuinely cannot be built without a change outside the fence,
    STOP and say which path you need and why, instead of making the change.
"""

DRY_SOLID_RULES = """\
## Code quality — DRY and SOLID are reviewed afterwards
DRY
  - Before writing anything, search the symbol index and the surrounding code for
    something that already does the job, and call it. Never paste a second copy
    of logic that exists, and never "improve" your private copy of it.
  - A helper two nodes would both want belongs where the graph puts it (usually
    the shared foundation module), not in both.
  - Repeated literals, paths and magic numbers become one named constant.

SOLID
  - Single responsibility: this node does the one job its description states. A
    class that grows a second job means a node is missing from the graph — say so
    in your summary rather than building it here.
  - Open/closed: extend through the interfaces the graph already names; do not
    edit a stable dependency to special-case your node.
  - Liskov: an implementation honours its base contract exactly — no strengthened
    preconditions, no silent no-ops.
  - Interface segregation: expose only what the dependents listed above actually
    call. No speculative API for callers that do not exist.
  - Dependency inversion: depend on the abstraction the graph names, not on a
    concrete type further down. Injection over globals, singletons, or reaching
    across modules.
"""

DONE = """\
## Definition of done
{items}
"""

RETRY = """\
## This is a RETRY — the previous attempt was rejected
{feedback}
"""

CONTEXT_POINTER = """\
## Context
The repository context for this task — layout, existing symbols, the relevant
file bodies and the plan excerpt — is in `{path}`.
READ THAT FILE FIRST. It exists so that you reuse what is already there.
"""

REVIEW = """\
# Review one node's implementation

A single graph node was just implemented by another agent. Judge the diff below
on four things only, in this order:

1. SCOPE — does the diff do exactly this node's job and nothing else? Work
   belonging to another node, opportunistic refactors, unrelated renames and
   speculative API all count as failures even when the file is allowed.
2. DRY — does it duplicate logic, constants or parsing that already exists in
   this repository? Name the existing thing it should have called.
3. SOLID — single responsibility, honest interfaces, dependencies on
   abstractions rather than concretions, no strengthened preconditions.
4. UNFILLED CALLS — dependencies that do not exist yet must be declared and left
   unfilled with a `HARNESS-STUB(<name>)` marker. They must not be quietly
   implemented, and they must not be silently dropped.

Do NOT ask for extra features, extra tests beyond the node's description, or
stylistic rewrites. Missing polish is not a failure; scope creep is.

## The node
kind: {kind}
name: {name}
description:
{description}

## The fence it was given
{fence}

## The diff
```diff
{diff}
```

Answer in exactly this form:

VERDICT: PASS
or
VERDICT: REVISE
FINDINGS:
- <one line per problem, naming the file and what to do instead>
"""


@dataclass
class Brief:
    node: Node
    core: str
    context: str

    def full(self) -> str:
        return self.core if not self.context else f"{self.core}\n{self.context}"

    def delivered(self, context_path: str | None) -> str:
        """The text handed to the backend; context may live in a file instead."""
        if not self.context:
            return self.core
        if context_path is None:
            return self.full()
        return self.core + "\n" + CONTEXT_POINTER.format(path=context_path)


def _bullets(lines: Sequence[str], empty: str = "  (none)") -> str:
    return "\n".join(f"  - {line}" for line in lines) if lines else empty


def _indent(text: str, prefix: str = "    ") -> str:
    return textwrap.indent(text.strip() or "(none)", prefix)


def _one_line(text: str, limit: int = 220) -> str:
    flat = " ".join(text.split())
    return flat if len(flat) <= limit else flat[: limit - 1] + "…"


class PromptBuilder:
    """Builds the node brief and the review prompt."""

    def __init__(self, graph: Graph, language_hint: str = "c++"):
        self.graph = graph
        self.language_hint = language_hint

    def build(
        self,
        node: Node,
        scope: Scope,
        sections: Sequence[Section],
        stub_sites: Sequence[StubSite] = (),
        verify_cmds: Sequence[str] = (),
        feedback: str = "",
        order_note: str = "",
    ) -> Brief:
        deps = self.graph.dependencies(node.index)
        existing = [d for d in deps if d.implemented]
        missing = [d for d in deps if not d.implemented]
        callers = self.graph.dependents(node.index)

        parts = [MISSION.format(
            name=node.name,
            kind=node.kind,
            module=scope.module or "(none in graph)",
            module_dir=scope.module_dir or "(unmapped)",
            description=_indent(node.comment),
            order_note=f" {order_note}" if order_note else "",
        )]

        parts.append(DEPENDENCIES.format(
            existing=_bullets([f"{d.name} ({d.kind}): {_one_line(d.comment)}" for d in existing]),
            missing=_bullets([
                f"{d.name} ({d.kind}) — leave `{marker_for(d.name)}`: {_one_line(d.comment)}"
                for d in missing
            ]),
            marker_shape=marker_for("<node name>"),
            stub_example=STUB_EXAMPLES.get(self.language_hint, STUB_EXAMPLES["c++"]),
            callers=_bullets([f"{c.name} ({c.kind}): {_one_line(c.comment)}" for c in callers]),
        ))

        if stub_sites:
            parts.append(STUB_SITES.format(
                marker=marker_for(node.name),
                sites="\n\n".join(s.render() for s in stub_sites),
            ))

        parts.append(FENCE.format(fence=scope.describe()))
        parts.append(DRY_SOLID_RULES)

        items = ["The node's description is fully implemented — no part deferred."]
        if missing:
            items.append("Every not-yet-built dependency is declared and left unfilled with its marker.")
        items.append("The project still builds; nothing that worked before is broken.")
        if verify_cmds:
            items.append("These commands pass: " + "; ".join(verify_cmds))
        items.append(
            "You finish with a short summary: files touched, interfaces you committed to, "
            "stubs you left, and anything the graph itself got wrong."
        )
        parts.append(DONE.format(items=_bullets(items)))

        if feedback:
            parts.append(RETRY.format(feedback=feedback.strip()))

        context = "\n".join(s.render() for s in sections)
        return Brief(node=node, core="\n".join(parts), context=context)

    def review_prompt(self, node: Node, scope: Scope, diff: str) -> str:
        return REVIEW.format(
            kind=node.kind,
            name=node.name,
            description=_indent(node.comment),
            fence=_indent(scope.describe()),
            diff=diff,
        )


def parse_review(text: str) -> tuple[bool, list[str]]:
    """(passed, findings) from a review reply; an unparseable reply passes."""
    idx = text.upper().rfind("VERDICT:")
    if idx < 0:
        return True, []
    tail = text[idx:]
    first_line = tail.splitlines()[0].upper()
    passed = "PASS" in first_line
    findings = [
        line.strip().lstrip("-* \t")
        for line in tail.splitlines()[1:]
        if line.strip().startswith(("-", "*")) and line.strip().lstrip("-* \t")
    ]
    return passed, findings[:20]
