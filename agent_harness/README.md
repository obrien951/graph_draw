# graph agent harness

Walks a `graph_io` dependency graph and deploys one agent per node — a coding
agent for a `Module`/`Class`/`Function`, an acquisition agent for an `Artifact`.

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

## Language

The graph, the fence and the walk are language-neutral; a handful of brief
details are not. Those live in `languages.py`, keyed by a short id
(`rust`, `c++`, `python`, `typescript`):

* the `HARNESS-STUB` / `HARNESS-PARTIAL` example shown in the brief
  (`unimplemented!(...)` for Rust, `Q_UNIMPLEMENTED()` for C++, `raise
  NotImplementedError` for Python),
* the "a body that …" wording in the no-op check,
* the build-artefact globs unioned into the ignore list so `target/`,
  `node_modules/` etc. never look like agent output or trip the fence,
* the verify commands the CLI suggests when none are configured,
* which source suffixes count as a neighbour's context.

Resolution order: `--language` → the scope config's `"language"` key →
autodetection from the repo's marker files (`Cargo.toml` → rust,
`CMakeLists.txt` → c++, `pyproject.toml` → python, …) → `c++` as the historical
default. The run header prints which one won and how.

```jsonc
// sentiment_aggregator.scope.json
{
  "language": "rust",
  ...
}
```

## Published reference data — fetch, don't reconstruct

A small local model asked to implement a node whose spec names a *published*
artefact — the VADER lexicon, the Loughran–McDonald word lists, an ISO table, a
stopword list — tends to write it out from training memory. The result looks
right and is quietly wrong: invented scores, missing entries.

Two mechanisms push back:

1. **The brief says so.** When a node's description mentions a known dataset
   kind (or the config lists resources), the brief carries a "fetch it, do not
   reconstruct it from memory" rule, and the review pass gets a sixth check
   that rejects a hand-authored table standing in for a real file.
2. **`reference_resources` in the scope config** names the real artefacts —
   the intended field is `search`, a query, not a hard-coded URL:

   ```jsonc
   "reference_resources": [
     {
       "name": "VADER lexicon",
       "search": "vader_lexicon.txt cjhutto vaderSentiment raw github",
       "path": "src/sentiment/data/vader_lexicon.txt",
       "note": "tab-separated: token<TAB>mean<TAB>std<TAB>[raw ratings]",
       "modules": ["sentiment"]
     }
   ]
   ```

   Each entry is listed in the brief of every node it applies to (omit
   `modules` for all). With `--fetch-refs` the harness **runs the query through
   a search engine**, ranks the results, rewrites a GitHub/GitLab file page to
   its raw URL, and downloads the best match into `<state-dir>/refs/`. It fails
   open at every step: no search engine, no usable result, or a download that
   403s just leaves the query and the candidate URLs in the brief for the
   agent. A module entry may carry its own `reference_resources` list. A bare
   `url` is still honoured (used as a single candidate).

## Artifact nodes

`reference_resources` is a side-channel. When a published file is a real
dependency — something a module cannot be built or tested without — it should
be a **node**, with edges from the things that need it, exactly like a code
dependency. That node's `kind` is `Artifact`:

```jsonc
{
  "kind": "Artifact",
  "name": "vader_lexicon",
  "comment": "The VADER sentiment lexicon: token<TAB>mean<TAB>std<TAB>[raw ratings].",
  "search": "vader_lexicon.txt cjhutto vaderSentiment VADER sentiment lexicon raw github",
  "path": "src/sentiment/data/vader_lexicon.txt",
  "sha256": "…",            // optional; the gate enforces it if present
  "license": "MIT"          // optional
}
```

An Artifact node is **not implemented by writing code**. Its agent is given the
`artifact_search` skill (`agent_harness/skills/artifact_search.md`) and a brief
that says: run the search, pick the primary source from the results, download
the real file, verify it, place it at `path`, and write
`<path>.provenance.json` (search query, source URL, retrieval time, sha256,
bytes, license). Differences from a code node:

* the fence is exactly `path` and its `.provenance.json` sidecar — no module
  allow-list, no `src/lib.rs` — and the added-lines budget is lifted (a
  published data file is thousands of lines);
* the walk order comes from the edges, and an edge into an Artifact is always
  ordered dependency-first (a file cannot be stubbed);
* the **code-review stage is replaced** by a deterministic check — file
  present, non-empty, sha256 matches if pinned — that is fail-closed like the
  rest of the gate; the build/test commands still run;
* a genuine blocker (source behind a login, no clean primary copy) is a
  `HARNESS-PARTIAL(<node>)` marker in the provenance file, never a fabricated
  data file.

With `--fetch-refs` the harness runs each Artifact node's search **before the
node's turn**, seeds the ranked results into the brief, and — when a result
clearly matches — places the file at `path` with a provenance sidecar so the
agent turn is a verification rather than a from-scratch download.

`--search-url` (or `$HARNESS_SEARCH_URL`, or `"search_url"` in the scope
config) swaps the engine: a form endpoint that takes `q=` (default: DuckDuckGo
lite), or a URL template with `{query}` (a SearXNG `/search?…&format=json`
instance works). `python -m agent_harness.references "<query>"` prints what a
query returns, for tuning a graph's `search` strings by hand.

`--kind Artifact` / `--skip` / `--only` select them like any other node.

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

## Model hosts

Two Tailscale boxes serve models; both are providers in
`~/.config/opencode/opencode.json`, so `--backend opencode --model <provider>/<name>`
reaches either. `tools/resume_qwem_build.sh <host>` picks one.

| host | server | ctx | notes |
|---|---|---|---|
| `felnor` | llama-swap, `felnor:8080` | 32768 | 12 models; `--context-chars 55000` |
| `son-of-felnor` | plain llama.cpp, `son-of-felnor:8080` | 65536 (4 slots) | one model; `--context-chars 100000` |

`son-of-felnor` serves a single gguf and llama.cpp reports its id as the file's
absolute path. `sync-llama-models.py` (in `~/.config/opencode/`) regenerates
the `son-of-felnor` provider's model list from that live `/v1/models` response
and uses the path verbatim as the config key — so right now the only working
`--model` value is the ugly one:
`son-of-felnor//home/joe/Downloads/Qwen3-Coder-30B-A3B-Instruct-UD-Q8_K_XL.gguf`
(verified directly: a clean alias like `son-of-felnor/Qwen3-Coder-30B-A3B...`
fails with a server error before the request ever reaches llama.cpp — opencode
resolves `-m` against its own config keys client-side, it does not fall back to
matching the display `name`). llama.cpp itself ignores the wire `model` field
in single-model mode, so a hand-renamed key in opencode.json *would* also work
— but `sync-llama-models.py` will overwrite it back to the raw path on its next
run, so don't bother unless you also want to maintain that by hand.

As of 2026-08-29, son-of-felnor runs Qwen3-Coder-30B-A3B-Instruct
(UD-Q8_K_XL, MoE, ~3B active params, NOT a reasoning model — `reasoning: false`
in opencode.json) instead of the earlier Qwen3.8-27B. felnor's Qwen3.8 builds
remain reasoning models: they return `reasoning_content` alongside `content`,
so budget output tokens for thinking as well as code.

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

# 4. the shared contract first, on the strong model, then freeze it
./graph_agent.py --graph graphs/graph_draw.planned.json \
    --scope-config harness.scope.json --plan-file IMPLEMENTATION_PLAN.md \
    --backend opencode --model felnor/kat-coder-v2.5-q6 --agent build \
    --only "ir::Repo" --only "ir::Module" --only "ir::Type" --only "ir::Function" \
    --only LanguageAnalyzer --only GraphBuilder --commit

# 5. everything else, cheaper, consuming the frozen contract, reviewed
./graph_agent.py --graph graphs/graph_draw.planned.json \
    --scope-config harness.scope.json --plan-file IMPLEMENTATION_PLAN.md \
    --backend opencode --model felnor/qwen3-coder-30b --agent build \
    --strong-model felnor/kat-coder-v2.5-q6 --strong-kind Module \
    --freeze 'graph_lang/**' \
    --review-backend llama-server --review-model qwen3-coder-30b \
    --review-server-url http://felnor:8080/v1 \
    --commit --resume --stop-on-failure
```

`--stop-on-failure` is worth having on the first real re-run: the previous run
carried on through 43 nodes after the tree had already stopped compiling.

Four nodes the graph marks `implemented: true` still carry Part B change
instructions — `graph_analyze`, `repo_to_graph`, `graph_draw_tests` and
`RepoAnalyzer`. They are skipped by default; pass them explicitly
(`--force graph_analyze`) or use `--include-implemented`.

## The gate

`gate.py` is the one place that decides whether a node may be marked
implemented. The traversal calls it; nothing else does; a model cannot opt out
of it. That is the reason the harness is a Python program rather than a prompt.

```
      build/test commands  ->  review agent  ->  implemented = true
              (Verifier)        (Reviewer)
```

Cheap and deterministic first, expensive and probabilistic second: a diff that
does not compile is never sent to a review model. Both stages run **per node,
per attempt**, inside the walk, and a refusal at either stage becomes the
feedback for the next attempt.

Before the first node, the gate also verifies the tree as it stands. A repo that
is already broken makes every node fail for reasons that are not the node's
fault, so the walk refuses to start instead.

**The reviewer is dispatched by the harness, not requested by the agent.** When
a `--review-backend` is configured, every node that builds gets its diff read by
a second model, which answers `VERDICT: PASS` or `VERDICT: REVISE` with
findings; findings are fed verbatim into the retry.

`review.py` fails **open** in four places — no backend, empty diff, unreachable
endpoint, unparseable reply — so an advisory reviewer can never stall a run.
`--require-review` inverts that: silence becomes a refusal, and a node cannot be
marked done unless a reviewer actually read its diff and passed it. Use it for
unattended runs; leave it off when the review endpoint is flaky.

The run summary marks what actually vouched for each node:

```
--- summary ---   [V]=build gate passed  [R]=reviewed by an agent
ok   [VR] Class    TomlDocument (2 files, 41s)
ok   [V-] Function rustlex::blank (1 files, 22s)
FAIL [V-] Module   graph_lang_rust — review requested changes (3 finding(s))
```

`state.json` records the same two booleans per node, so a completion claim can
be audited after the fact rather than taken on trust.

## What the 2026-08-19 run taught us

That run walked all 43 nodes, reported 39 done, and produced a tree that did not
compile. Full analysis in `../PART_B_STATUS.md`. Five things changed here as a
result; the first is the one that mattered.

**1. A node is not done until the project builds.** The `Verifier` already
existed and was already called inside the walk, but was given no commands, so a node was marked done whenever its agent
exited without a scope violation. `harness.scope.json` now carries a `verify`
block with configure/build/ctest, and the CLI **refuses to write
`implemented: true` when nothing is verifying it**:

```
error: refusing to write implemented=true with nothing verifying it.
```

Override with `--allow-unverified-graph`, or turn flag-writing off with
`--no-update-graph`. `--plan-only` and `--backend dry-run` are exempt, since
they write nothing.

**2. A failed node is rewound.** Previously a node that failed every attempt
left its half-written files on disk, and the next node treated them as real.
That is where the duplicate `graph_lang_rust/src/` tree came from. The runner
now snapshots once per node and restores on terminal failure. `--no-revert-on-failure`
restores the old behaviour when you want to inspect the wreckage.

**3. Shared contracts can be frozen mid-run.** 43 agents independently invented
`RepoFileIndex` — seven declarations, all incompatible — because each was fenced
to its own module and none could see the others. Build the shared types first,
then lock them:

```bash
./graph_agent.py ... --only ir::Repo --only LanguageAnalyzer   # build the contract
./graph_agent.py ... --freeze 'graph_lang/**' --resume         # everyone else consumes it
```

`--freeze` adds paths to the protected list for that run, so a later agent that
tries to redefine the contract has the edit reverted and is told why.

**4. Structural nodes can use a bigger model.** A small model will write a
plausible interface it cannot then implement. Tier it:

```bash
./graph_agent.py ... --model qwen3-coder-30b \
    --strong-model kat-coder-v2.5-q6 --strong-kind Module --strong-node GraphBuilder
```

Leaf functions stay on the cheap model; modules and named structural nodes get
the expensive one.

**5. Work the graph cannot express needs its own step.** The graph's vocabulary
is `Module`/`Class`/`Function`/`Artifact`, so build wiring, CLI flags and
documentation had no node and were simply never attempted. Use `--pre-cmd` /
`--post-cmd`:

```bash
./graph_agent.py ... --pre-cmd 'git add -A && git commit -qm baseline' \
                     --post-cmd 'cmake --build build -j4'
```

For agent-driven non-code work, the better fix is to give it a node — a CLI
change belongs to a `Function main` node under the `repo_to_graph` module.

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
| `languages.py` | per-language brief details + repo autodetection |
| `references.py` | reference resources + Artifact-node acquisition: the search engine, `--fetch-refs` retrieval, the sha256 check |
| `skills.py` / `skills/` | instruction blocks pasted into a brief (`artifact_search`) |
| `scope.py` | the fence: policy, resolution, audit |
| `workspace.py` | git snapshot, change detection, revert, commit |
| `context.py` | repo tree, symbol index, file bodies, plan excerpt |
| `prompts.py` | the brief and the review prompt |
| `stubs.py` | the `HARNESS-STUB(...)` protocol and its index |
| `backends.py` | opencode, OpenAI-compatible, llama-cli, dry-run |
| `patchformat.py` | file-block protocol for non-agentic models |
| `verify.py` / `review.py` | the two checks: build commands, and a second model on the diff |
| `gate.py` | composes them into the one gate the traversal calls (review → sha256 check for Artifact nodes) |
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
