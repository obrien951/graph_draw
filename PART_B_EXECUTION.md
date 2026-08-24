# Part B execution plan — how to invoke agents for the remaining work

> **Resume:** `claude --resume 3cedc4d2-db1d-4292-8004-0a7713e94e48` (run from `~/repos/graph_draw`)
>
> **Status at time of writing:** tree is at HEAD and green (11/11 ctest). Recovery steps 1–3 of
> `PART_B_STATUS.md` are done. `graphs/graph_draw.planned.json` reads 24 implemented / 43 not.
> **Every phase B1–B13 is open.** Nothing from the 2026-08-19 run survives outside `.salvage/`.

---

## Context

Part B is the language-plugin refactor of `graph_draw` described in `IMPLEMENTATION_PLAN.md`.
The first attempt drove all 43 graph nodes through `agent_harness` on a 9B local model with no
build gate; it reported 39 done and left a tree that did not link. `PART_B_STATUS.md` has the
autopsy. The harness has since been fixed (build gate, deterministic review dispatch, rollback
on failure, `--freeze`, model tiering), but **the harness was never the whole answer** — two
structural facts limit what it can own:

1. **The graph cannot express B0, B10, B11 or B12.** All 43 nodes carry a task id in the range
   B1–B9. Build wiring, CLI flags and documentation have no `Module`/`Class`/`Function` node,
   so the harness will never attempt them regardless of configuration.
2. **The shared contract has to exist before it can be consumed.** 43 fenced agents each
   invented their own `RepoFileIndex` — seven incompatible declarations — because no contract
   existed to consume. Freezing works only once there is something to freeze.

So Part B needs three different kinds of invocation, in order. This document is the how.

### Does the plan use the harness?

`IMPLEMENTATION_PLAN.md` never mentions `agent_harness/`, `graph_agent.py` or
`harness.scope.json` — its six "harness" hits are Part C's C++ comparison tool and a throwaway
serializer rig. The coupling runs one way only:

- `harness.scope.json:1` names `graphs/graph_draw.planned.json` and Part B explicitly, and
  fences per module exactly as Part B's layout requires
- `agent_harness/context.py:144` `plan_section()` extracts the `**B1 — …**` block from the
  markdown into each node's brief
- all 43 unimplemented nodes carry a resolvable task id (verified)

The wiring is real; only the plan's prose is unaware of it. **B11 should fix that** — see
Phase 3.

---

## Phase 1 — the spine (B1 → B2 → B3), Claude Code, not a local model

24 of the 43 nodes. This is the shared IR, the `LanguageAnalyzer` interface, `GraphBuilder`,
and the `CppAnalyzer` extraction. It is where correctness compounds and where the last attempt
failed hardest, so it does not go to a local model.

| Task | Nodes |
|---|---|
| B1 | `graph_lang`, `LanguageAnalyzer`, `RepoFileIndex`, `buildRepoFileIndex`, `sourcetext::blankC`, `sourcetext::leadingComment`, `sourcetext::matchingBrace`, `LanguageAnalyzer::{id,detect,analyze}`, `RepoFileIndex::{named,withSuffix}` |
| B2 | `ir::{Repo,Module,Type,Function}`, `GraphBuilder`, `GraphBuilder::build` |
| B3 | `graph_lang_cpp`, `CppAnalyzer`, `CppAnalyzer::{detect,analyze}`, `RepoAnalyzer::{analyzeToIr,setLanguage}` |

### Invocation

Work on a branch, one task per agent run, committing between them:

```bash
git checkout -b part-b-spine

# B1 — headless, one task
claude -p "$(cat <<'EOF'
Implement task B1 from IMPLEMENTATION_PLAN.md in this repo.

Read first: IMPLEMENTATION_PLAN.md (the B1 block), graph_analyze/repoanalyzer.cpp,
graphs/README.md, and .salvage/KEEP/matching_brace.{h,cpp}.

Scope: create graph_lang/ only. Do not touch graph_analyze/, graph_core/, graph_io/,
tests/test_repoanalyzer.cpp, or graphs/.

graph_lang links Qt Core ONLY. It must never include or link graph_core -- that
constraint is the entire point of the module.

Acceptance, all of which must pass before you report done:
  cmake -S . -B build && cmake --build build -j4
  cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure
plus a new test asserting the length-preservation invariant blankC(s).size() == s.size().

.salvage/KEEP/matching_brace.cpp is prior-attempt code: it is off-by-one against its own
documented contract and has a potential out-of-bounds read. Treat it as a sketch, not a source.
EOF
)"
```

Then the same shape for B2 and B3, adjusting the task id and the acceptance criteria.
Alternatively run interactively and dispatch a subagent per task — the point is one task per
agent, with a build gate between them, not the transport.

### Feed the spine agent the salvage

`.salvage/KEEP/` holds the only genuinely good output of the last run. Hand it over **with its
known defects named**, so it is used as reference rather than pasted:

| File | Known defects to fix |
|---|---|
| `ir.h` / `ir.cpp` (~300 lines, matches the planned design) | `Repo::nextId() const` mutates a non-`mutable` member (will not compile); `Module::addChildModuleLabels` assigns where it should append |
| `matching_brace.h` / `.cpp` | off-by-one vs its documented contract; potential OOB read |

### B3's acceptance is the one that matters

B3 is pure motion — moving working code, changing no behaviour. It has a mechanical test:

```bash
# test_repoanalyzer.cpp must pass with ZERO edits
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure

# analyzer parity against a FIXED input tree
./tools/analyzer_parity.sh
```

**Corrected 2026-08-24 after B1 landed.** The obvious version of this check — scan the working
tree and diff against `graphs/graph_draw.generated.json` — is wrong. That baseline was scanned
before Part B added `graph_lang/`, so the working tree now legitimately yields 27 extra nodes
(verified: 27 added, **0 removed**, all of them graph_lang's own symbols). Comparing against it
would fail for reasons unrelated to analyzer behaviour, and worse, would tempt someone to
regenerate the baseline and destroy the contract.

`tools/analyzer_parity.sh` holds the *input* fixed instead: it checks out the pre-refactor
commit in a git worktree and scans **that** with the freshly built binary, comparing as sorted
multisets. Any difference is then a genuine behaviour change. It passes today (92 nodes, 110
edges), so it is a live gate rather than an aspiration.

Also required by the plan: `repoanalyzer.cpp` under ~120 lines, and `graph_analyze` no longer
links `graph_io` (it links it today at `graph_analyze/CMakeLists.txt:9-13`).

### Close the phase

The build gate — not agent exit status — decides the flags. Only after the commands above are
green:

```bash
python3 - <<'EOF'
import json
SPINE = {"graph_lang","LanguageAnalyzer","RepoFileIndex","buildRepoFileIndex",
 "sourcetext::blankC","sourcetext::leadingComment","sourcetext::matchingBrace",
 "LanguageAnalyzer::id","LanguageAnalyzer::detect","LanguageAnalyzer::analyze",
 "RepoFileIndex::named","RepoFileIndex::withSuffix",
 "ir::Repo","ir::Module","ir::Type","ir::Function","GraphBuilder","GraphBuilder::build",
 "graph_lang_cpp","CppAnalyzer","CppAnalyzer::detect","CppAnalyzer::analyze",
 "RepoAnalyzer::analyzeToIr","RepoAnalyzer::setLanguage"}
p = "graphs/graph_draw.planned.json"
d = json.load(open(p))
n = sum(1 for x in d["nodes"] if x["name"] in SPINE and not x.get("implemented"))
for x in d["nodes"]:
    if x["name"] in SPINE: x["implemented"] = True
json.dump(d, open(p,"w"), indent=2)
print(f"flipped {n} spine nodes to implemented")
EOF
git add -A && git commit -m "B1-B3: shared contract, IR, GraphBuilder, CppAnalyzer extraction"
```

**Then point the harness's context at the new contract** — `harness.scope.json:context_files`
still lists `graph_analyze/repoanalyzer.{h,cpp}`. Replace with the contract headers
(`graph_lang/languageanalyzer.h`, `graph_lang/ir/ir.h`, `graph_lang/repofileindex.h`) so every
leaf agent sees what it must consume. Without this edit the leaves will reinvent it again.

---

## Phase 2 — the leaves (B4–B9), the harness, frozen and gated

The remaining 19 nodes. These are genuinely independent, well-specified, and individually
small — which is what the harness is good at.

| Task | Nodes |
|---|---|
| B5 | `TomlDocument`, `TomlDocument::{parseText,keys,stringList,tableArrayCount}` |
| B4 | `graph_lang_rust`, `rustlex::{blank,leadingDoc}` |
| B9 | `graph_merge`, `GraphMerger`, `GraphMerger::merge` |
| B6 | `RustAnalyzer`, `RustAnalyzer::{detect,analyze}` |
| B7 | `RustFileItems`, `scanRustFile` |
| B8 | `LabelResolver`, `LabelResolver::{observe,resolve}` |

### Step 2a — one node first, as a calibration probe

No local model here has been validated against this task. `TomlDocument` is the right probe:
pure logic, no Qt GUI, no dependency on anything unbuilt, and `IMPLEMENTATION_PLAN.md` calls it
"highest value-per-hour test file in the plan" with precise must-handle / must-not-handle lists.

```bash
./graph_agent.py --graph graphs/graph_draw.planned.json \
    --scope-config harness.scope.json --plan-file IMPLEMENTATION_PLAN.md \
    --backend opencode --model felnor/qwen3-coder-30b --agent build \
    --only TomlDocument --only "TomlDocument::parseText" \
    --freeze 'graph_lang/**' --freeze 'graph_lang_cpp/**' --freeze 'graph_analyze/**' \
    --require-review \
    --review-backend llama-server --review-model qwen3-coder-30b \
    --review-server-url http://felnor:8080/v1 \
    --state-dir .harness-probe-qwen30b --stop-on-failure
```

Then **read the diff yourself.** If it is weak, rerun the identical command against
`felnor/kat-coder-v2.5-q5l` (262k ctx) or `felnor/ornith-35b-q6` (196k ctx, 35B) into a
different `--state-dir`, and compare. Twenty minutes here replaces a guess about model choice
with evidence. Note the failed run used `Qwen3.8-9B-Q8_0` on the *llamacpp* endpoint — none of
the felnor models has ever actually been tried on this repo.

### Step 2b — the rest, once a model has earned it

```bash
./graph_agent.py --graph graphs/graph_draw.planned.json \
    --scope-config harness.scope.json --plan-file IMPLEMENTATION_PLAN.md \
    --backend opencode --model <the model that won> --agent build \
    --freeze 'graph_lang/**' --freeze 'graph_lang_cpp/**' --freeze 'graph_analyze/**' \
    --require-review \
    --review-backend llama-server --review-model qwen3-coder-30b \
    --review-server-url http://felnor:8080/v1 \
    --commit --resume --stop-on-failure
```

Why each flag earns its place:

- **`--freeze`** — the direct fix for seven `RepoFileIndex` declarations. A leaf agent that
  tries to redefine the contract has the edit reverted and is told why.
- **`--require-review`** — `review.py` fails *open* in four places (no backend, empty diff,
  unreachable endpoint, unparseable reply). This inverts that: silence becomes refusal. Worth
  knowing that nearly every Part B node creates only new files, so an empty-diff pass would
  have rubber-stamped almost everything.
- **`--stop-on-failure`** — the last run carried on through 43 nodes after the tree had already
  stopped compiling.
- **`--resume`** — leaves the probe's work in place.
- The build gate itself needs no flag: `harness.scope.json` carries the `verify` block, and the
  CLI now refuses to write `implemented=true` with nothing verifying it.

Expect to run this more than once. `--stop-on-failure` plus `--resume` makes that cheap.

---

## Phase 3 — B10, B11, B12 as ordinary work, outside the graph

Once the libraries exist and build. These have no nodes and get none; they are a normal coding
task for a capable agent:

- **B10 — CLI.** `repo_to_graph [--lang cpp|rust] [--merge <existing.json>] [--dry-run]` via
  `QCommandLineParser`. Default output path stays `<root>/graphs/<name>.generated.json`, and
  `--merge` must never write to its own input unless explicitly named — this tool once
  overwrote the curated `graph_draw.json`. Summary gains `kept N · added M · stale K`.
- **B11 — docs.** `repoanalyzer.h:6-24` (still says C++-only), `analyze/main.cpp:10-17`,
  `graphs/README.md`, memory notes — **and `IMPLEMENTATION_PLAN.md` itself**, which should
  finally acknowledge `agent_harness/` as the mechanism Part B is executed by. Reconcile
  `graphs/graph_draw.planned.json` against what actually got built; where reality diverged, the
  divergence is the interesting artifact.
- **B12 — polish.** Dynamic layout bands, label elision, and two pre-existing C++ lexer bugs
  (digit separators `1'000'000`, raw strings `R"(he said "hi")"`).

**B0** (commit the baseline) is already satisfied — `tests/test_repoanalyzer.cpp` is tracked and
byte-identical to HEAD.

**B13** (AI-drawn babytrack graph) needs Part A ≈A11 and is out of scope until then.

---

## Verification

Run after every phase, not just at the end:

```bash
cmake -S . -B build && cmake --build build -j4
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure   # expect 11/11 before B4
python3 -m unittest discover -s tests -p 'test_agent_harness.py'  # expect 33 OK
```

The B3 multiset comparison above is the single most important check in Part B: it proves the
refactor changed structure without changing behaviour. Run it before starting Phase 2 — if the
generated graph has drifted, every later node builds on a broken analyzer.

If a macOS Qt build hits `qyieldcpu.h:37 implicitly declaring library function '__yield'`, add
`-include arm_acle.h` to the compile flags.
