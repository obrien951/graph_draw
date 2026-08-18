# graph agent harness

Walks a `graph_io` dependency graph and deploys one coding agent per node.

The graphs in `graphs/` are implementation plans: each node's `comment` is the
prompt for the thing it represents, `implemented` says whether it exists, and an
edge `A → B` means *A depends on B*. This harness reads that plan and builds it.

```
./graph_agent.py --graph graphs/graph_draw.planned.json \
    --scope-config harness.scope.json --plan-file IMPLEMENTATION_PLAN.md \
    --backend opencode --model felnor/qwen3-coder-30b --agent build
```

## The walk: foot to leaf

Default order (`--order root-first`) is topological on the dependency edges, so
a node is implemented **before** the things it calls exist. Each agent therefore
declares what it needs from a not-yet-built dependency, writes the call, and
leaves the body unfilled with a marker naming the node that owes it:

```cpp
// HARNESS-STUB(ir::Repo): declared here, implemented by its own node.
ir::Repo RustAnalyzer::analyze(const RepoFileIndex& index) const {
    Q_UNIMPLEMENTED();
    return {};
}
```

When the walk reaches `ir::Repo`, the harness greps the tree for
`HARNESS-STUB(ir::Repo)` and puts every call site — with surrounding lines — in
that agent's brief, so the dependency is implemented against real usage instead
of a guess. Interfaces flow downward; implementations flow back up. Unfilled
calls still open at the end are listed in the run summary.

`--order leaf-first` inverts it: dependencies land first and callers see real
code. Use it when the interfaces are already settled and you want no stubs.

## Preventing scope creep

Four layers, because a prompt alone does not hold:

1. **Declared.** Every node resolves to an explicit allow-list of paths, derived
   from the Module that contains it in the graph (`contains`/`provides` edges)
   plus whatever `--scope-config` adds. The agent never picks its own scope.
2. **Stated.** The allow-list, the deny-list and the budgets (max changed files,
   max added lines) are printed into the brief verbatim.
3. **Enforced.** The working tree is snapshotted with `git stash create` before
   each attempt; afterwards the real diff is audited against the fence and
   anything that escaped is reverted — modified files restored, new files
   deleted. `--on-violation` picks the policy: `revert-retry` (default, retry
   with the violation list as feedback), `revert`, `warn`, `fail`.
4. **Reviewed.** With `--review-backend`, a second model reads only the diff and
   answers `VERDICT: PASS` or `REVISE` plus findings, judging scope, DRY, SOLID
   and whether the unfilled-call protocol was honoured. Findings go straight
   back into the retry. Missing polish is explicitly not a failure; scope creep
   is.

Protected paths (regression-contract tests, the graphs, `memory/`, the harness
itself) are refused even when they sit inside an allowed directory.

## DRY and SOLID

Two mechanisms rather than one instruction:

* the brief carries a **symbol index** of everything already defined in the repo
  plus the bodies of the node's module and its implemented neighbours, so the
  agent can call what exists instead of writing a second copy;
* the brief carries a per-principle checklist, and the review pass names the
  existing function a duplicate should have called.

## Context

* `--plan-file` — the task id in a node's comment (`B1`, `A7`, `C0`) is looked
  up in that markdown file and the matching section is pasted into the brief.
* `--context-file` / `--context-glob` — always include these files.
* `context_files` in the scope config — the same thing, per project.
* `--context-chars` caps the total; briefs over `--inline-limit` spill their
  context to `.harness/work/<node>/context.md`, which the brief tells the agent
  to read first.

## Backends

| `--backend` | what it is | how work lands |
|---|---|---|
| `opencode` (default) | `opencode run --dir <repo> -m <model> --agent <name> --auto` | the agent edits the tree itself |
| `llama-server` | any OpenAI-compatible endpoint — a local `llama-server`, or the llama-swap box configured in `~/.config/opencode/opencode.json` | model answers in file blocks, the harness writes them |
| `llama-cli` | a local `.gguf` through llama.cpp's `llama-cli` | same file-block protocol |
| `dry-run` | writes the brief, launches nothing | nothing changes |

`--model` and `--agent` are passed straight through, so `--model
felnor/kat-coder-v2.5-q6 --agent build` picks the model *and* the opencode agent
definition. The review pass takes its own `--review-backend/--review-model/
--review-agent`, so a cheap local model can review an expensive one's work (or
the reverse).

File-block backends answer with whole files:

```
<<<FILE graph_lang/sourcetext.cpp>>>
...entire file...
<<<END>>>
```

Anything they write still goes through the same scope audit.

## Run control

State, briefs, prompts, agent logs, verify logs and backups live under
`--state-dir` (default `.harness/`):

```
.harness/state.json                     per-node status, attempts, changed files
.harness/work/<node>/brief.attempt1.md  exactly what the agent was told
.harness/logs/<node>.attempt1.log       what the agent said back
.harness/backup/<node>/                 pre-run copies of untracked files
```

`--resume` skips nodes already `done`; `--reset` starts over. `--plan-only`
prints the walk order with each node's scope and the stubs it will leave.
`--pause` confirms before each node, `--only`/`--skip`/`--kind`/`--max-nodes`
narrow the selection, `--commit` commits per node, and a successful node flips
`implemented: true` in the graph file (turn off with `--no-update-graph`).

`--verify-cmd` runs after each node; a failure feeds the output back as retry
feedback rather than being reported at the end.

## Building this repo's Part B

`harness.scope.json` is the fence for `graphs/graph_draw.planned.json`: module
directories, the per-module test files each node may add, and the protected
regression contract `tests/test_repoanalyzer.cpp`.

```bash
# 1. look at the order and the fences, launch nothing
./graph_agent.py --graph graphs/graph_draw.planned.json \
    --scope-config harness.scope.json --plan-only

# 2. read the exact brief one node would get
./graph_agent.py --graph graphs/graph_draw.planned.json \
    --scope-config harness.scope.json --plan-file IMPLEMENTATION_PLAN.md \
    --backend dry-run --only "sourcetext::blankC"

# 3. build one leaf for real, with a build+test gate
./graph_agent.py --graph graphs/graph_draw.planned.json \
    --scope-config harness.scope.json --plan-file IMPLEMENTATION_PLAN.md \
    --backend opencode --model felnor/qwen3-coder-30b --agent build \
    --only "sourcetext::blankC" \
    --verify-cmd "cmake -S . -B build" --verify-cmd "cmake --build build -j"

# 4. the whole of Part B, reviewed, committing as it goes
./graph_agent.py --graph graphs/graph_draw.planned.json \
    --scope-config harness.scope.json --plan-file IMPLEMENTATION_PLAN.md \
    --backend opencode --model felnor/kat-coder-v2.5-q6 --agent build \
    --review-backend llama-server --review-model qwen3-coder-30b \
    --review-server-url http://felnor:8080/v1 \
    --verify-cmd "cmake --build build -j" --commit --resume
```

Four nodes the graph marks `implemented: true` still carry Part B change
instructions — `graph_analyze`, `repo_to_graph`, `graph_draw_tests` and
`RepoAnalyzer`. They are skipped by default; pass them explicitly
(`--force graph_analyze`) or use `--include-implemented`.

## Tests

```bash
python3 -m unittest discover -s tests -p 'test_agent_harness.py'
```

The end-to-end cases drive a real `Harness` over a scratch git repository with a
scripted backend, so the fence, the revert, the retry and the graph update are
exercised for real rather than mocked.

## Layout

| file | job |
|---|---|
| `graph_agent.py` | thin entry point, nothing else |
| `graphmodel.py` | the graph, dependency queries, walk order |
| `scope.py` | the fence: policy, resolution, audit |
| `workspace.py` | git snapshot, change detection, revert, commit |
| `context.py` | repo tree, symbol index, file bodies, plan excerpt |
| `prompts.py` | the brief and the review prompt |
| `stubs.py` | the `HARNESS-STUB(...)` protocol and its index |
| `backends.py` | opencode, OpenAI-compatible, llama-cli, dry-run |
| `patchformat.py` | file-block protocol for non-agentic models |
| `verify.py` / `review.py` | the two quality gates |
| `state.py` / `runner.py` | resumable state, and the walk itself |
| `cli.py` | argument parsing and wiring |

## Limits

* The fence is path-based. An agent can still write the wrong thing in a file it
  legitimately owns — that is what the review pass and `--verify-cmd` are for.
* Reverting uses git, so `--no-scope-guard` (no git) means no enforcement.
* Interfaces invented for a stub are a contract the later agent inherits. If the
  early agent invents a bad signature, the later one is told it may object —
  read the summaries.
* Nodes run one at a time. Independent subtrees could run in parallel; they do
  not yet.
