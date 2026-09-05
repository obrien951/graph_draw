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

from . import languages, skills
from .context import Section
from .graphmodel import KIND_ARTIFACT, Graph, Node
from .references import ReferenceResource, for_module
from .scope import Scope
from .stubs import PARTIAL_MARKER, StubSite, marker_for, partial_marker_for

MISSION = """\
# Implement one graph node: {name}

You are one agent in a graph-driven build. The dependency graph of this project
is the plan: every node is a Module, Class, Function or Artifact, and an edge
`A -> B` means A depends on B. You have been given exactly one node. Another
agent gets each of the others.{order_note}

## The node
kind          : {kind}
name          : {name}
owning module : {module} -> {module_dir}

## Its description — this is the specification, follow it literally
{description}
"""

ARTIFACT_MISSION = """\
# Acquire one graph node: {name}

This is an **Artifact** node, not a code node. You are not writing an
implementation — you are retrieving a real, published file that the rest of
the build depends on, verifying it, and placing it where the graph says.

## The node
kind          : {kind}
name          : {name}
owning module : {module} -> {module_dir}

## What to acquire — follow this literally
{description}

## The concrete target
{resource}

## Nodes that depend on this artifact
{callers}
"""

ARTIFACT_DONE = """\
## Definition of done
{items}

Write the file and its provenance now.
"""

DEPS_EXISTING = """\
## Dependencies that already exist — call them, never re-create them
{existing}
"""

DEPS_MISSING_CODE = """\
## Code dependencies that DO NOT exist yet — leave unfilled calls
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
"""

DEPS_MISSING_ARTIFACT = """\
## Artifact dependencies not yet on disk — load them from their path anyway
These are published files that their own Artifact node will download and place
at the path shown. They are ordered before you in the walk, so the file is
normally already there — but write the load against the path regardless. Do
NOT inline a copy of the data and do NOT leave a HARNESS-STUB for it: it is a
file, not a call.
{missing}
"""

DEPS_CALLERS = """\
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

REFERENCE_DATA_RULE = """\
## Published reference data — fetch it, do not reconstruct it from memory
If this node's specification names a *published* external artefact — a
sentiment or financial word list (VADER, Loughran-McDonald), a stopword list,
an ISO/Unicode table, a country/currency/exchange mapping, an RFC grammar, a
standard test corpus — you must load the *real* data, not a version you write
out from your own training knowledge.

A hand-authored table that approximates a public dataset is a correctness
defect: the values are subtly wrong, entries are missing, and nothing downstream
can be trusted. The review pass rejects it.

Do this instead, in order of preference:
  1. If a real copy is on disk (see the list below, or the paths the spec
     names), load it from there.
  2. If you have a web-search or fetch tool, download the genuine file from its
     authoritative source and commit it (or a documented, deterministic sample
     of it) at the path the code loads from.
  3. If you can do neither and the spec only needs a *small bundled seed*,
     derive that seed by sampling the real file — and leave a
     `{partial_marker}` marker naming the full file you could not fetch, so a
     later pass finishes it. Do NOT invent the entries.
"""

REFERENCE_DATA_LIST = """\
### Reference resources for this node
These are the real artefacts this node's specification depends on. Use them
instead of reconstructing the data:
{resources}
"""

DONE = """\
## Definition of done
{items}

{directive}
"""

ESCAPE_HATCH = """\
## If part of this node is genuinely infeasible this turn
This run allows a deliberate partial completion — but it is not a shortcut, and
it is not for "this is hard" or "I am running low on turns." It is for a
specific, nameable blocker: a dependency's real shape will only be known once
its own (separate, not-yet-built) node exists, an external tool or file this
needs is unavailable, or the specification itself conflicts with what is
already on disk. If that happens:
  - Implement everything you can for real — no throwing, no fake success.
  - At the exact gap, leave a comment: `{partial_marker}: <one clear
    sentence naming the specific blocker>`.
  - Say so plainly in your summary.
Do not use this to avoid finishing something merely tedious. A `{partial_marker}`
marker left for a reason a reviewer would call flimsy is treated the same as
leaving the node unimplemented.

Shape of a declared partial:
```
{partial_example}
```
"""

RESUME_PARTIAL = """\
## This node already carries partial work from an earlier turn
A previous attempt (this run or an earlier one) implemented part of this node
for real and stopped at a declared blocker:
  {partial_reason}
Read what is already on disk before touching anything. If that blocker is now
resolved (its dependency exists now, the missing tool is available, whatever
it was), finish the job and remove the `{partial_marker}(...)` marker. If it
is still blocked, leave the working part alone — do not rewrite it from
scratch — and only touch what the blocker actually affects.
"""

RETRY = """\
## This is a RETRY — the previous attempt was rejected
{feedback}

Make the fix now.
"""

FIX = """\
# Fix a rejected change: {name}

The change you just made in this same conversation was rejected:

{problem}

Fix exactly this. Keep everything else about what you already wrote — do not
start over from scratch, and do not touch anything beyond what fixing this
specific problem requires.

Never run `rm`, `git clean`, or any other destructive command on `.harness/`
or on anything outside the path you were told to work in — not even if
`git status` shows it as untracked. Untracked is normal here; nothing in this
build is committed yet, and `.harness/` is the build tool's own working
directory, not part of your change. If a path shows up as forbidden, the fix
is to stop writing to it or move that code into your own allowed paths — it
is never to delete it.

Fix it now.
"""

CONTEXT_POINTER = """\
## Context
The repository context for this task — layout, existing symbols, the relevant
file bodies and the plan excerpt — is in `{path}`.
READ THAT FILE FIRST. It exists so that you reuse what is already there.

Write the changes now.
"""

NOOP_CHECK = """\
# Before implementing — is this node's job already done?

Node: {name} ({kind})
Specification: {description}

Read the current code in the allowed paths below and answer one question: does
the codebase ALREADY do everything this specification asks for, such that
writing anything here would be pure duplication?

This is almost always NO. Answer NOOP only when you can point at the exact
existing function/method that already does the full job — not "something
similar exists," not "most of it is covered," not "a helper could easily be
adapted." A `HARNESS-STUB(<name>)` marker, a body that {unfinished_phrase},
or any TODO left in the node's own
code is NEVER a no-op, by definition — that IS the unimplemented work this
node exists to do. Do not confuse "a reference implementation exists in
.salvage/" with "this is already done" either: salvage material is explicitly
unverified and untrusted until a node's own agent turn adapts it for real.

Do not edit, create or delete any file. This is a read-only check.

## Allowed paths (for reference only — do not write to them here)
{fence}

Answer in exactly this form:

VERDICT: NOOP
EVIDENCE: <the exact file:line or function name that already does this, and how>
or
VERDICT: IMPLEMENT

Answer now.
"""

DOUBLE_CHECK = """\
# Double-check one graph node's implementation: {name}

Determine the correctness of the implementation of: {description}

...and rectify any discrepancies between the implementation and the
specification above. Re-read the files you are responsible for — do not trust
your memory of the previous turn — check each requirement against what is
actually on disk, and fix anything that has drifted. If the implementation
already matches the specification, make no changes and say so; do not invent
extra work.

A `HARNESS-STUB(<name>)` marker left in place is NOT a discrepancy to fix. It
is a deliberate placeholder for a dependency that is a separate node elsewhere
in the graph, built in its own dedicated turn later. Filling it in here does
that other node's job out of turn and the two passes can end up in conflict.
Only correct what THIS node's own description above promised.

## Scope fence — unchanged from the implementation pass
{fence}
Anything you change outside the allowed paths is reverted automatically.

## Still true
{items}

Fix it now.
"""

REVIEW = """\
# Review one node's implementation

This codebase is built one graph node per turn, not one file or one class per
turn: a Class node's own diff may legitimately leave ONE OF ITS OWN METHODS
still throwing/unimplemented behind a `HARNESS-STUB(<name>)` marker, because
that method's body is a SEPARATE Function node elsewhere in the graph, given
to a different agent in its own dedicated turn. That is correct structure, not
an incomplete implementation — do not flag it as missing work.

A single graph node was just implemented by another agent. Judge the diff below
on six things only, in this order:

1. SCOPE — does the diff do exactly this node's job and nothing else? Work
   belonging to another node, opportunistic refactors, unrelated renames and
   speculative API all count as failures even when the file is allowed.
2. DRY — does it duplicate logic, constants or parsing that already exists in
   this repository? Name the existing thing it should have called.
3. SOLID — single responsibility, honest interfaces, dependencies on
   abstractions rather than concretions, no strengthened preconditions.
4. UNFILLED CALLS — a dependency that does not exist yet must be declared and
   left unfilled with a `HARNESS-STUB(<name>)` marker (see above — this is
   normal, not a defect). It must not be quietly implemented, and it must not
   be silently dropped.
5. DECLARED PARTIAL WORK — a `HARNESS-PARTIAL(<name>): <reason>` marker is this
   node's own agent honestly saying "everything else here is real and tested;
   this specific, named piece is not, and here is why." Accept it exactly like
   an unfilled dependency (#4) PROVIDED the reason is a real, specific blocker
   (a genuinely unavailable dependency, tool, or file) — REVISE only if the
   reason is vague, unconvincing, or reads like an excuse to skip tedious work
   that was actually feasible.
6. RECONSTRUCTED REFERENCE DATA — if the node's description names a *published*
   dataset, dictionary, standard table or word list and the diff contains a
   large hand-authored table approximating it (invented sentiment scores, a
   half-remembered category list) rather than code that loads the real file, or
   a small sample explicitly derived from it, REVISE and say the real source
   should have been fetched. A genuine file committed at the load path, or a
   `HARNESS-PARTIAL` marker naming the file that could not be fetched, is fine.

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

File the review now.
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

    def __init__(self, graph: Graph, language_hint: str = "c++",
                 reference_resources: Sequence[ReferenceResource] = ()):
        self.graph = graph
        self.language_hint = language_hint
        self.language = languages.get(language_hint)
        self.reference_resources = tuple(reference_resources)

    def build(
        self,
        node: Node,
        scope: Scope,
        sections: Sequence[Section],
        stub_sites: Sequence[StubSite] = (),
        verify_cmds: Sequence[str] = (),
        feedback: str = "",
        order_note: str = "",
        allow_partial: bool = False,
        existing_partial: str | None = None,
    ) -> Brief:
        if node.kind == KIND_ARTIFACT:
            return self._artifact_brief(node, scope, sections, verify_cmds,
                                        feedback, allow_partial, existing_partial)
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

        if existing_partial:
            parts.append(RESUME_PARTIAL.format(
                partial_reason=existing_partial, partial_marker=PARTIAL_MARKER,
            ))

        missing_code = [d for d in missing if d.kind != KIND_ARTIFACT]
        missing_art = [d for d in missing if d.kind == KIND_ARTIFACT]
        parts.append(DEPS_EXISTING.format(
            existing=_bullets([self._existing_dep_line(d) for d in existing])))
        if missing_code:
            parts.append(DEPS_MISSING_CODE.format(
                missing=_bullets([self._missing_dep_line(d) for d in missing_code]),
                marker_shape=marker_for("<node name>"),
                stub_example=self.language.stub_example))
        if missing_art:
            parts.append(DEPS_MISSING_ARTIFACT.format(
                missing=_bullets([self._missing_dep_line(d) for d in missing_art])))
        parts.append(DEPS_CALLERS.format(
            callers=_bullets([f"{c.name} ({c.kind}): {_one_line(c.comment)}" for c in callers])))

        if stub_sites:
            parts.append(STUB_SITES.format(
                marker=marker_for(node.name),
                sites="\n\n".join(s.render() for s in stub_sites),
            ))

        parts.append(FENCE.format(fence=scope.describe()))
        parts.append(DRY_SOLID_RULES)

        node_refs = for_module(self.reference_resources, scope.module)
        if node_refs or self._spec_names_a_dataset(node):
            parts.append(REFERENCE_DATA_RULE.format(partial_marker=partial_marker_for(node.name)))
        if node_refs:
            parts.append(REFERENCE_DATA_LIST.format(
                resources="\n".join(r.describe() for r in node_refs),
            ))

        if allow_partial:
            parts.append(ESCAPE_HATCH.format(
                partial_marker=PARTIAL_MARKER,
                partial_example=self.language.partial_example,
            ))

        items = ["The node's description is fully implemented — no part deferred."
                 if not allow_partial else
                 "The node's description is fully implemented, or the one part that "
                 "genuinely is not carries a HARNESS-PARTIAL marker naming why."]
        if any(d.kind != KIND_ARTIFACT for d in missing):
            items.append("Every not-yet-built code dependency is declared and left unfilled with its marker.")
        if any(d.kind == KIND_ARTIFACT for d in missing):
            items.append("Each not-yet-fetched Artifact dependency is loaded from the path its "
                         "node will place it at — no inlined copy, no stub marker.")
        items.append("The project still builds; nothing that worked before is broken.")
        if verify_cmds:
            items.append("These commands pass: " + "; ".join(verify_cmds))
        items.append(
            "You finish with a short summary: files touched, interfaces you committed to, "
            "stubs you left, and anything the graph itself got wrong."
        )
        parts.append(DONE.format(items=_bullets(items), directive="Write the changes now."))

        if feedback:
            parts.append(RETRY.format(feedback=feedback.strip()))

        context = "\n".join(s.render() for s in sections)
        return Brief(node=node, core="\n".join(parts), context=context)

    def _existing_dep_line(self, d: Node) -> str:
        if d.kind == KIND_ARTIFACT:
            res = ReferenceResource.from_node(d)
            where = f"the file at `{res.path}`" if res.path else "a downloaded file"
            return (f"{d.name} (Artifact): {where} — load it from that path, "
                    f"never inline a copy. {_one_line(d.comment, 120)}")
        return f"{d.name} ({d.kind}): {_one_line(d.comment)}"

    def _missing_dep_line(self, d: Node) -> str:
        if d.kind == KIND_ARTIFACT:
            res = ReferenceResource.from_node(d)
            where = f"`{res.path}`" if res.path else "the path its node names"
            fmt = f" [{res.note}]" if res.note else ""
            return f"{d.name} → load from {where}{fmt}: {_one_line(d.comment, 140)}"
        return f"{d.name} ({d.kind}) — leave `{marker_for(d.name)}`: {_one_line(d.comment)}"

    def _artifact_brief(self, node: Node, scope: Scope, sections: Sequence[Section],
                        verify_cmds: Sequence[str], feedback: str,
                        allow_partial: bool, existing_partial: str | None) -> Brief:
        res = ReferenceResource.from_node(node)
        callers = self.graph.dependents(node.index)
        parts = [ARTIFACT_MISSION.format(
            name=node.name, kind=node.kind,
            module=scope.module or "(standalone)",
            module_dir=scope.module_dir or "(unmapped)",
            description=_indent(node.comment),
            resource=_indent(res.describe()),
            callers=_bullets([f"{c.name} ({c.kind}): {_one_line(c.comment)}" for c in callers]),
        )]

        if existing_partial:
            parts.append(RESUME_PARTIAL.format(
                partial_reason=existing_partial, partial_marker=PARTIAL_MARKER))

        parts.append(FENCE.format(fence=scope.describe()))
        parts.append("## How to do this\n\n" + skills.load(skills.ARTIFACT_SEARCH, demote=1))

        items = [
            f"The real file is saved at `{res.path or '<the path the node names>'}`, "
            "byte-for-byte as published (no re-encoding, re-sorting or trimming).",
        ]
        if res.sha256:
            items.append(f"Its sha256 is {res.sha256}.")
        items.append(f"`{res.path or '<path>'}.provenance.json` records source URL, "
                     "retrieval time, sha256, bytes and license.")
        if verify_cmds:
            items.append("The project still builds — these pass: " + "; ".join(verify_cmds))
        if allow_partial:
            items.append(f"...or, if the file genuinely cannot be obtained, a "
                         f"`{partial_marker_for(node.name)}: <blocker>` marker in the "
                         f"provenance file says why — never a fabricated data file.")
        items.append("Your summary names the source, the sha256, the license and any transform.")
        parts.append(ARTIFACT_DONE.format(items=_bullets(items)))

        if feedback:
            parts.append(RETRY.format(feedback=feedback.strip()))

        context = "\n".join(s.render() for s in sections)
        return Brief(node=node, core="\n".join(parts), context=context)

    #: Phrases in a node's spec that usually mean "load a published dataset".
    #: Used only to decide whether to spend brief space on REFERENCE_DATA_RULE
    #: for a node the config listed no explicit resources for.
    _REFERENCE_HINTS = (
        "vader", "loughran", "mcdonald", "lexicon", "stopword", "stop-word",
        "stop word", "wordlist", "word list", "word-list", "dictionary",
        "sentiment word", "iso 3166", "iso 4217", "iso-3166", "iso-4217",
        "unicode data", "corpus", "gazetteer", "published list", "standard list",
        "reference table", "reference data", "canonical list",
    )

    def _spec_names_a_dataset(self, node: Node) -> bool:
        low = node.comment.lower()
        return any(hint in low for hint in self._REFERENCE_HINTS)

    def fix_prompt(self, node: Node, problem: str) -> str:
        return FIX.format(name=node.name, problem=problem.strip())

    def noop_check_prompt(self, node: Node, scope: Scope) -> str:
        return NOOP_CHECK.format(
            name=node.name, kind=node.kind,
            description=_indent(node.comment),
            fence=_indent(scope.describe()),
            unfinished_phrase=self.language.unfinished_phrase,
        )

    def double_check_prompt(self, node: Node, scope: Scope, verify_cmds: Sequence[str] = ()) -> str:
        items = ["The node's description is fully implemented — no part deferred.",
                 "The project still builds; nothing that worked before is broken."]
        if verify_cmds:
            items.append("These commands pass: " + "; ".join(verify_cmds))
        return DOUBLE_CHECK.format(
            name=node.name,
            description=_indent(node.comment),
            fence=_indent(scope.describe()),
            items=_bullets(items),
        )

    def review_prompt(self, node: Node, scope: Scope, diff: str) -> str:
        return REVIEW.format(
            kind=node.kind,
            name=node.name,
            description=_indent(node.comment),
            fence=_indent(scope.describe()),
            diff=diff,
        )


def parse_noop_verdict(text: str) -> tuple[bool, str]:
    """(is_noop, evidence) from a no-op-check reply.

    Unlike parse_review, this fails CLOSED: an unparseable, ambiguous, or
    missing verdict means "implement it" (is_noop=False), never "skip it".
    Silently skipping real work on a garbled response would be exactly the
    false-completion failure mode this whole harness exists to prevent.
    """
    idx = text.upper().rfind("VERDICT:")
    if idx < 0:
        return False, ""
    tail = text[idx:]
    first_line = tail.splitlines()[0].upper()
    if "NOOP" not in first_line:
        return False, ""
    evidence = ""
    for line in tail.splitlines()[1:]:
        stripped = line.strip()
        if stripped.upper().startswith("EVIDENCE:"):
            evidence = stripped.split(":", 1)[1].strip()
            break
    return True, evidence or "(no evidence given)"


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
