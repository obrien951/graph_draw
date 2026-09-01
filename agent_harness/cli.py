"""Command line: pick a graph, a backend, an agent and a model, and walk."""
from __future__ import annotations

import argparse
import subprocess
import sys
import textwrap
from dataclasses import replace
from pathlib import Path
from typing import Sequence

from .backends import BACKENDS, DEFAULT_SERVER_URL, DRY_RUN, OPENCODE, AgentSpec, build_backend
from .context import Budget, RepoContext
from .graphmodel import LEAF_FIRST, ORDERS, ROOT_FIRST, Graph, GraphError
from .prompts import PromptBuilder
from .review import Reviewer
from .runner import VIOLATION_POLICIES, Harness, HarnessOptions, ON_VIOLATION_RETRY, SafetyNetLost
from .scope import ScopeConfig, ScopeResolver
from .state import DONE, RunState
from .stubs import StubIndex
from .verify import Verifier
from .workspace import GitWorkspace, WorkspaceError

DESCRIPTION = """\
Walk a graph_io dependency graph and deploy one coding agent per node.

The walk is foot-to-leaf by default: a node is implemented before the things it
calls exist, so each agent declares what it needs from its dependencies and
leaves the bodies unfilled with a HARNESS-STUB(<node>) marker. When the walk
reaches that dependency, its agent is handed every call site waiting on it.
"""

EPILOG = """\
examples:
  # see the plan without launching anything
  graph_agent.py --graph graphs/graph_draw.planned.json --backend dry-run --plan-only

  # build this repo's Part B with opencode against a local llama-swap model
  graph_agent.py --graph graphs/graph_draw.planned.json \\
      --backend opencode --model felnor/qwen3-coder-30b --agent build \\
      --scope-config harness.scope.json --plan-file IMPLEMENTATION_PLAN.md \\
      --verify-cmd "cmake --build build" --verify-cmd "ctest --test-dir build"

  # one node, reviewed by a second model on llama-server
  graph_agent.py --graph graphs/graph_draw.planned.json --only TomlDocument \\
      --backend opencode --model felnor/kat-coder-v2.5-q6 \\
      --review-backend llama-server --review-model qwen3-coder-30b \\
      --review-server-url http://felnor:8080/v1

  # no agent CLI at all: a local .gguf answers with file blocks
  graph_agent.py --graph graphs/graph_draw.planned.json --backend llama-cli \\
      --gguf ~/models/Qwen3.8-9B-Q8_0.gguf --only "sourcetext::blankC"
"""


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="graph_agent.py", description=DESCRIPTION, epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    g = p.add_argument_group("graph and repository")
    g.add_argument("--graph", required=True, help="graph_io JSON file to build from")
    g.add_argument("--repo", default=".", help="repository root the agents work in (default: .)")
    g.add_argument("--order", choices=ORDERS, default=ROOT_FIRST,
                   help=f"{ROOT_FIRST}: callers first, stub what they call (default). "
                        f"{LEAF_FIRST}: dependencies first.")
    g.add_argument("--scope-config", help="JSON scope/fence policy (see harness.scope.json)")
    g.add_argument("--plan-file", help="markdown plan; the task id in a node's comment is "
                                       "extracted from it into the prompt")

    s = p.add_argument_group("selection")
    s.add_argument("--only", action="append", default=[], metavar="NAME",
                   help="build just this node (repeatable); implies --force")
    s.add_argument("--skip", action="append", default=[], metavar="NAME", help="never build this node")
    s.add_argument("--kind", action="append", default=[], choices=["Module", "Class", "Function"],
                   help="restrict to these node kinds")
    s.add_argument("--force", action="append", default=[], metavar="NAME",
                   help="build a node even though the graph marks it implemented")
    s.add_argument("--include-implemented", action="store_true",
                   help="build every node, implemented or not")
    s.add_argument("--max-nodes", type=int, default=0, help="stop after N nodes (0 = no limit)")

    a = p.add_argument_group("agent")
    a.add_argument("--backend", choices=BACKENDS, default=OPENCODE)
    a.add_argument("--model", help="model name, e.g. felnor/qwen3-coder-30b for opencode")
    a.add_argument("--agent", help="agent name passed to the backend (opencode --agent)")
    a.add_argument("--agent-binary", help="path to the backend binary")
    a.add_argument("--variant", help="opencode --variant (reasoning effort)")
    a.add_argument("--server-url", default=DEFAULT_SERVER_URL,
                   help=f"OpenAI-compatible endpoint for llama-server (default {DEFAULT_SERVER_URL})")
    a.add_argument("--api-key", help="bearer token for the endpoint, if it wants one")
    a.add_argument("--gguf", help="model file for --backend llama-cli")
    a.add_argument("--ctx-size", type=int, default=32768)
    a.add_argument("--max-tokens", type=int, default=8192)
    a.add_argument("--temperature", type=float, default=0.2)
    a.add_argument("--agent-timeout", type=int, default=3600, help="seconds per agent run")
    a.add_argument("--agent-arg", action="append", default=[],
                   help="extra argument passed through to the backend (repeatable)")

    r = p.add_argument_group("review pass (DRY / SOLID / scope)")
    r.add_argument("--review-backend", choices=BACKENDS,
                   help="backend for the review pass (default: same as --backend — a node "
                        "reviews its own diff unless you point this at a different backend)")
    r.add_argument("--review-model", help="default: same as --model")
    r.add_argument("--review-agent", help="default: same as --agent")
    r.add_argument("--review-server-url")
    r.add_argument("--review-gguf")
    r.add_argument("--no-review", action="store_true", help="disable review entirely")
    r.add_argument("--require-review", action="store_true",
                   help="a node may not be marked done unless a review agent actually read "
                        "its diff and passed it. Makes an unreachable or unintelligible "
                        "reviewer a failure instead of a silent pass")

    c = p.add_argument_group("context")
    c.add_argument("--context-file", action="append", default=[], metavar="PATH",
                   help="always include this file in the prompt (repeatable)")
    c.add_argument("--context-glob", action="append", default=[], metavar="GLOB",
                   help="always include files matching this glob (repeatable)")
    c.add_argument("--context-chars", type=int, default=90_000, help="context budget per prompt")
    c.add_argument("--inline-limit", type=int, default=60_000,
                   help="briefs longer than this spill their context to a file the agent reads")

    v = p.add_argument_group("verification and safety")
    v.add_argument("--verify-cmd", action="append", default=[], metavar="CMD",
                   help="shell command that must pass after each node (repeatable)")
    v.add_argument("--verify-timeout", type=int, default=1800)
    v.add_argument("--attempts", type=int, default=2, help="attempts per node (default 2)")
    v.add_argument("--on-violation", choices=VIOLATION_POLICIES, default=ON_VIOLATION_RETRY,
                   help="what to do when the agent edits outside its scope")
    v.add_argument("--no-scope-guard", action="store_true",
                   help="do not snapshot or revert (no git required, no fence enforcement)")
    v.add_argument("--stop-on-failure", action="store_true")
    v.add_argument("--commit", action="store_true", help="git commit after each successful node")
    v.add_argument("--commit-template", default="harness: implement {kind} {name}")
    v.add_argument("--no-update-graph", action="store_true",
                   help="do not flip implemented=true in the graph file")
    v.add_argument("--no-revert-on-failure", action="store_true",
                   help="leave a failed node's half-written files on disk (default: rewind "
                        "them, so the next node never builds on broken code)")
    v.add_argument("--allow-unverified-graph", action="store_true",
                   help="permit flipping implemented=true with no --verify-cmd configured. "
                        "Off by default: an unverified flag is a false completion claim")
    v.add_argument("--freeze", action="append", default=[], metavar="GLOB",
                   help="treat these paths as protected for this run. Use after a shared "
                        "contract lands so later nodes consume it instead of reinventing it")

    d = p.add_argument_group("self-correction")
    d.add_argument("--confirm-changes", action="store_true",
                   help="after each implementation attempt, send one blunt follow-up in "
                        "the SAME opencode session: 'Were the changes made? Make sure you "
                        "actually did what was asked.' Cheap compared to --double-check (no "
                        "fresh session, no spec re-read) — for the specific failure of a "
                        "reasoning model stopping mid-plan without ever calling a write tool. "
                        "Only opencode has a resumable session to continue; ignored on other "
                        "backends. Off by default: it is still one more agent call per "
                        "attempt, real wall-clock cost on a slow model.")
    d.add_argument("--fix-rounds", type=int, default=0, metavar="N",
                   help="instead of immediately reverting/restarting a scope violation or a "
                        "failed gate (build/test or review), first try up to N fixer turns in "
                        "the SAME opencode session: hand the agent the specific violation or "
                        "gate rejection and ask it to fix exactly that, keeping the rest of "
                        "its work. Falls through to the normal revert/retry behavior if still "
                        "unresolved after N rounds — bounded, not a way to loop forever. Each "
                        "fixer turn also gets its own scope re-audit. Off by default (0): a "
                        "violation reverts to the last good snapshot and a failed gate retries "
                        "from a fresh attempt, same as before this option existed.")
    d.add_argument("--double-check", type=int, default=0, metavar="N",
                   help="after a node's implementation passes its gate, ask the SAME agent "
                        "to re-read its diff against the node's specification and rectify any "
                        "discrepancies, N times (default: 0/off). Only runs for agentic "
                        "backends (opencode); a round that breaks the fence or the gate is "
                        "reverted rather than kept")
    d.add_argument("--allow-partial", action="store_true",
                   help="an escape hatch: an agent may leave a HARNESS-PARTIAL(<node>): "
                        "<reason> marker at a genuinely infeasible piece of its own job "
                        "instead of being forced to either fully finish or fail outright. "
                        "The node is kept (not reverted) and marked 'partial' in state.json, "
                        "but is NOT flagged implemented in the graph, so a later --resume "
                        "retries it with the existing partial work as context. Build/test "
                        "still must pass either way — this waives spec completeness, not "
                        "correctness. Off by default: without it, a HARNESS-PARTIAL marker "
                        "is treated as an incomplete attempt and retried.")
    d.add_argument("--noop-check", action="store_true",
                   help="before spending a full write-capable attempt, ask the same agent a "
                        "cheap read-only question: does the codebase already do this node's "
                        "whole job? Only accepted when the agent names the exact existing "
                        "code doing it AND the build/test gate still passes; a HARNESS-STUB "
                        "or any placeholder body is defined as never a no-op. A true verdict "
                        "flags the node implemented without writing anything, marked 'noop' "
                        "in state.json for auditability. Off by default: an extra agent call "
                        "on every node is real wall-clock cost on a slow model, worth paying "
                        "only when you suspect the graph and the code have drifted apart.")

    m = p.add_argument_group("model tiering")
    m.add_argument("--strong-model", help="a second, more capable model for structural nodes")
    m.add_argument("--strong-backend", choices=BACKENDS, help="backend for --strong-model")
    m.add_argument("--strong-gguf", help="model file when --strong-backend is llama-cli")
    m.add_argument("--strong-node", action="append", default=[], metavar="NAME",
                   help="build this node with --strong-model (repeatable)")
    m.add_argument("--strong-kind", action="append", default=[], metavar="KIND",
                   choices=["Module", "Class", "Function"],
                   help="build every node of this kind with --strong-model (repeatable)")

    t = p.add_argument_group("non-code steps")
    t.add_argument("--pre-cmd", action="append", default=[], metavar="CMD",
                   help="shell command to run before the walk (repeatable). For work the "
                        "graph cannot express: committing a baseline, wiring the build")
    t.add_argument("--post-cmd", action="append", default=[], metavar="CMD",
                   help="shell command to run after the walk (repeatable)")

    o = p.add_argument_group("run control")
    o.add_argument("--state-dir", default=".harness", help="logs, briefs, backups and run state")
    o.add_argument("--resume", action="store_true", help="skip nodes already marked done in state")
    o.add_argument("--reset", action="store_true", help="discard previous run state")
    o.add_argument("--plan-only", action="store_true", help="print the walk order and exit")
    o.add_argument("--pause", action="store_true", help="confirm before each node")
    return p



def _wrap(label: str, items, width: int = 96) -> str:
    body = ", ".join(items)
    indent = " " * 14
    return textwrap.fill(body, width=width, initial_indent=f"       {label}: ",
                         subsequent_indent=indent)


def _selected(graph: Graph, args, order: Sequence[int], state: RunState) -> list[int]:
    only = {n.lower() for n in args.only}
    skip = {n.lower() for n in args.skip}
    force = {n.lower() for n in args.force} | only
    kinds = set(args.kind)
    chosen: list[int] = []
    for i in order:
        node = graph.node(i)
        low = node.name.lower()
        if only and low not in only:
            continue
        if low in skip:
            continue
        if kinds and node.kind not in kinds:
            continue
        if node.implemented and not (args.include_implemented or low in force):
            continue
        if args.resume and state.status_of(node.name) == DONE:
            continue
        chosen.append(i)
    if args.max_nodes:
        chosen = chosen[: args.max_nodes]
    return chosen


def _agent_spec(args) -> AgentSpec:
    return AgentSpec(
        backend=args.backend, model=args.model, agent=args.agent, binary=args.agent_binary,
        server_url=args.server_url, api_key=args.api_key, gguf=args.gguf,
        ctx_size=args.ctx_size, max_tokens=args.max_tokens, temperature=args.temperature,
        timeout=args.agent_timeout, variant=args.variant, extra_args=tuple(args.agent_arg),
    )


def _strong_spec(args) -> AgentSpec | None:
    """The alternate, more capable agent, if one was configured."""
    if not (args.strong_model or args.strong_gguf):
        return None
    spec = _agent_spec(args)
    return replace(spec,
                   backend=args.strong_backend or spec.backend,
                   model=args.strong_model or spec.model,
                   gguf=args.strong_gguf or spec.gguf)


def _run_steps(label: str, commands, cwd) -> int:
    """Run the --pre-cmd / --post-cmd hooks, stopping at the first failure."""
    for command in commands:
        print(f"{label}: $ {command}")
        code = subprocess.run(command, shell=True, cwd=str(cwd)).returncode
        if code != 0:
            print(f"error: {label} command failed ({code}): {command}", file=sys.stderr)
            return code
    return 0


def _review_spec(args) -> AgentSpec | None:
    """Review defaults to the SAME backend/model/agent as the build pass — a
    node reviewing its own diff — unless --review-* overrides it or
    --no-review turns it off. Only --no-review disables review; there is no
    longer an implicit "unset --review-backend means off".
    """
    if args.no_review:
        return None
    return AgentSpec(
        backend=args.review_backend or args.backend,
        model=args.review_model or args.model,
        agent=args.review_agent or args.agent,
        binary=args.agent_binary,
        server_url=args.review_server_url or args.server_url,
        api_key=args.api_key,
        gguf=args.review_gguf or args.gguf,
        ctx_size=args.ctx_size, max_tokens=args.max_tokens,
        temperature=args.temperature, timeout=args.agent_timeout,
    )


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root = Path(args.repo).resolve()

    try:
        graph = Graph.load(args.graph)
    except GraphError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    config = ScopeConfig.load(args.scope_config) if args.scope_config else ScopeConfig()
    verify_cmds = args.verify_cmd or list(config.verify)
    if args.freeze:
        # A frozen path is just a protected one, decided at run time rather than
        # in the config, so a contract can be locked the moment it lands.
        config = replace(config, deny=tuple(config.deny) + tuple(args.freeze))

    update_graph = not args.no_update_graph
    previewing = args.plan_only or args.backend == DRY_RUN
    if update_graph and not verify_cmds and not args.allow_unverified_graph and not previewing:
        print("error: refusing to write implemented=true with nothing verifying it.\n"
              "       The 2026-08-19 run marked 39 nodes done without ever compiling; the\n"
              "       flags were false and the tree did not build.\n"
              "       Give it a gate, e.g.:\n"
              "         --verify-cmd 'cmake --build build -j4'\n"
              "         --verify-cmd 'cd build && QT_QPA_PLATFORM=offscreen ctest'\n"
              "       or pass --no-update-graph, or override with --allow-unverified-graph.",
              file=sys.stderr)
        return 2
    context_files = list(dict.fromkeys(list(config.context_files) + args.context_file))

    if args.reset and args.resume:
        print("error: --reset and --resume contradict each other — --reset deletes the run "
              "state before --resume ever gets to check it, so every node (including ones "
              "already marked done) would be rebuilt from scratch. Drop one of the two.",
              file=sys.stderr)
        return 2

    state_dir = (root / args.state_dir) if not Path(args.state_dir).is_absolute() else Path(args.state_dir)
    state_path = state_dir / "state.json"
    if args.reset and state_path.exists():
        state_path.unlink()
    state = RunState.load(state_path)

    order, cyclic = graph.order(args.order)
    selection = _selected(graph, args, order, state)

    print(f"graph : {args.graph} — {graph.summary()}")
    print(f"repo  : {root}")
    print(f"order : {args.order} ({len(selection)} node(s) selected)")
    if cyclic:
        print(f"warning: {len(cyclic)} node(s) sit in a dependency cycle and were appended last: "
              f"{', '.join(graph.node(i).name for i in cyclic)}")

    scopes = ScopeResolver(graph, config)
    blocked = [(graph.node(i).name, culprit) for i in selection
               if (culprit := scopes.resolve(graph.node(i)).blocking_deny())]
    if blocked:
        print("error: these selected node(s) have their own directory shadowed by a "
              "deny/--freeze pattern — every attempt would burn a full agent run only to "
              "fail on a forbidden-path violation:", file=sys.stderr)
        for name, culprit in blocked:
            print(f"  {name}: shadowed by {culprit!r}", file=sys.stderr)
        print("A --freeze meant to protect an already-finished directory can shadow a node the "
              "graph later added inside it. Drop the offending --freeze, or exclude the node "
              "with --skip.", file=sys.stderr)
        return 2

    context = RepoContext(root, ignore=config.ignore, budget=Budget(total_chars=args.context_chars))
    if args.context_glob:
        context_files += context.matching(args.context_glob)

    if args.plan_only or args.backend == DRY_RUN:
        for position, i in enumerate(selection, 1):
            node = graph.node(i)
            scope = scopes.resolve(node)
            todo = [d.name for d in graph.unimplemented_dependencies(i)]
            print(f"{position:3d}. {node.kind:8s} {node.name}")
            print(_wrap("scope", scope.allow or ["(none)"]))
            print(_wrap("stubs", todo or ["(none)"]))
        if args.plan_only:
            return 0

    try:
        backend = build_backend(_agent_spec(args), root)
    except (ValueError, FileNotFoundError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    strong_spec = _strong_spec(args)
    try:
        strong_backend = build_backend(strong_spec, root) if strong_spec else None
    except (ValueError, FileNotFoundError) as exc:
        print(f"error: --strong-model: {exc}", file=sys.stderr)
        return 2

    review_spec = _review_spec(args)
    review_backend = build_backend(review_spec, root) if review_spec else None

    workspace = None
    if not args.no_scope_guard:
        try:
            workspace = GitWorkspace(root, ignore=config.ignore)
        except WorkspaceError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2

    prompts = PromptBuilder(graph)
    harness = Harness(
        graph=graph, root=root, backend=backend,
        scopes=scopes, context=context, prompts=prompts,
        workspace=workspace,
        verifier=Verifier(verify_cmds, root, timeout=args.verify_timeout),
        reviewer=Reviewer(review_backend, prompts, root),
        state=state, state_dir=state_dir,
        options=HarnessOptions(
            order=args.order, attempts=max(1, args.attempts), on_violation=args.on_violation,
            inline_limit=args.inline_limit, update_graph=update_graph,
            commit=args.commit, commit_template=args.commit_template, pause=args.pause,
            plan_path=args.plan_file, extra_context=tuple(context_files),
            stop_on_failure=args.stop_on_failure,
            revert_on_failure=not args.no_revert_on_failure,
            require_review=args.require_review,
            double_check_rounds=args.double_check,
            allow_partial=args.allow_partial,
            noop_check=args.noop_check,
            confirm_changes=args.confirm_changes,
            fix_rounds=args.fix_rounds,
        ),
        strong_backend=strong_backend,
        strong_nodes=tuple(args.strong_node),
        strong_kinds=tuple(args.strong_kind),
    )
    state.meta = {"graph": str(args.graph), "order": args.order,
                  "agent": backend.describe(),
                  "strong_agent": strong_backend.describe() if strong_backend else None,
                  "review": review_backend.describe() if review_backend else None,
                  "review_required": bool(args.require_review),
                  "verify": list(verify_cmds)}
    state.save()

    print(f"agent : {backend.describe()}")
    if review_backend:
        print(f"review: {review_backend.describe()}")
    if not selection:
        print("nothing to do")
        return 0

    if strong_backend:
        print(f"strong: {strong_backend.describe()}")
    print(f"verify: {'; '.join(verify_cmds) if verify_cmds else 'NONE (graph updates off)'}")
    print(f"gate  : {harness.gate().describe()}")
    broken_gate = harness.gate().misconfigured()
    if broken_gate:
        print(f"error: {broken_gate}", file=sys.stderr)
        return 2

    failed_step = _run_steps("pre", args.pre_cmd, root)
    if failed_step:
        return failed_step

    try:
        outcomes = harness.run(selection)
    except KeyboardInterrupt:
        print("\ninterrupted; state saved — rerun with --resume")
        return 130
    except SafetyNetLost as exc:
        print(f"\nerror: {exc}", file=sys.stderr)
        print("The harness's own state directory was deleted mid-run — no further revert "
              "can be trusted, so the walk stopped here rather than continuing on a "
              "compromised safety net. Inspect the tree by hand before resuming: state.json's "
              "\"done\" entries may no longer match what is actually on disk if their files "
              "were caught in the same deletion.", file=sys.stderr)
        return 3

    code = _report(harness, outcomes, state_dir, config)
    post = _run_steps("post", args.post_cmd, root)
    return code or post


def _report(harness: Harness, outcomes, state_dir: Path, config: ScopeConfig) -> int:
    print("\n--- summary ---   [V]=build gate passed  [R]=reviewed by an agent")
    failed = 0
    for o in outcomes:
        mark = {"done": "ok  ", "failed": "FAIL", "skipped": "skip",
                "partial": "part", "noop": "noop"}.get(o.status, o.status)
        # The reason names the ground the attempt was refused on (runner.REASON_*);
        # note is the human-readable detail. Only meaningful for a failed attempt —
        # a done node has no reason, and a partial node's note IS its own explanation.
        reason = f" ({o.reason})" if o.status == "failed" and o.reason else ""
        detail = f" — {o.note}" if o.note else ""
        dc = f" · double-checked {o.double_checked}x" if o.double_checked else ""
        # Show what actually vouched for a "done": an unbacked ok is the thing
        # that made the 2026-08-19 state file untrustworthy.
        gate = f"[{'V' if o.verified else '-'}{'R' if o.reviewed else '-'}]"
        print(f"{mark} {gate} {o.node.kind:8s} {o.node.name} "
              f"({len(o.changed)} files, {o.seconds:.0f}s){reason}{detail}{dc}")
        failed += o.status == "failed"
    outstanding = StubIndex(harness.root, ignore=config.ignore).outstanding()
    if outstanding:
        print("\nunfilled calls still open:")
        for name, sites in sorted(outstanding.items()):
            where = ", ".join(f"{s.path}:{s.line}" for s in sites[:4])
            print(f"  {name} — {len(sites)} site(s): {where}")
    print(f"\nstate, briefs and logs: {state_dir}")
    return 1 if failed else 0
