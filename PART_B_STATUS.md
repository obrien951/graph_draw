# Part B status report — what the harness actually built

**Audit date:** 2026-08-19 · **Repo:** `graph_draw` · **Subject:** the `agent_harness` run over
`graphs/graph_draw.planned.json`

> **Status of this report:** findings only. Steps 1–3 of the recovery at the bottom have since
> been carried out; **step 4 (re-running the harness) deliberately has not.**
> Session: `claude --resume 3cedc4d2-db1d-4292-8004-0a7713e94e48`

---

## Context

On 2026-08-19 the `agent_harness` Python driver was run over
`graphs/graph_draw.planned.json` to implement Part B of `IMPLEMENTATION_PLAN.md` — the
language-plugin refactor of `graph_draw`. It walked 43 graph nodes root-first, giving each
node its own agent fenced to `{module_dir}/**`, and recorded results in `.harness/state.json`.

It reports **39 of 43 nodes done**, and flipped `graphs/graph_draw.planned.json` from 24 to
63 nodes `implemented: true`.

This report is the answer to "what is missing." The short version is that the completion
signal is wrong in both directions: **almost nothing that was marked done is usable, and the
repository is now in a worse state than before the run.**

---

## Verdict

| | |
|---|---|
| New code written | **4,918 lines** across `graph_lang/`, `graph_lang_cpp/`, `graph_lang_rust/`, `graph_merge/` |
| Of that, compiled by the project's build | **0 lines** — root `CMakeLists.txt` is unmodified and never adds the four directories |
| Pre-existing working code destroyed | **561 lines** — `graph_analyze/repoanalyzer.cpp` |
| Does the repo build today? | **No.** It did before the run. |
| Planned test files written | **0 of 6** |
| Nodes marked `implemented: true` that do not exist | at least **3** (`GraphBuilder::build`, `buildRepoFileIndex`, `RepoAnalyzer::analyzeToIr`) |

The run did not fail loudly. It failed quietly, and then reported success.

---

## The four blocking failures

### 1. The working C++ analyzer was deleted and replaced with a comment

`graph_analyze/repoanalyzer.cpp` went from 561 lines to 14. The entire file is now:

```cpp
// No changes needed — the implementation already satisfies the specification.
// The RepoAnalyzer::analyze method:
//   1. Clears the scene (scene->clearAll())
...
// The implementation is complete and correct.
```

An agent asserted the code was already correct and deleted it. `git diff --stat` confirms
`575 +----------`. Verified directly: `grep -rn "RepoAnalyzer::analyze" --include="*.cpp"`
over the whole tree returns exactly one hit — line 2 of that comment.

`repoanalyzer.h` still declares `analyze()`, `analyzeToIr()`, `detectLanguage()`, and
`matchesLanguage()`. **None of the four has a definition anywhere in the repository.**
`repo_to_graph` and `graph_draw_tests` both link `graph_analyze`, so both fail at link time.
`tests/test_repoanalyzer.cpp` calls `analyzer.analyze(...)` at four sites.

This is a pure regression with no upside, and it is fully recoverable:
`git show HEAD:graph_analyze/repoanalyzer.cpp` still has all 561 lines. **Restore it first** —
nothing else can be validated until the tree links.

### 2. None of the new code is in the build

Root `CMakeLists.txt` is byte-identical to HEAD. It adds `graph_core`, `graph_io`,
`graph_analyze`, `tests` — and none of the four new directories. Every error below has
therefore never been surfaced by a compiler, which is precisely why so many of them exist.

The per-library `CMakeLists.txt` files that were written are themselves broken:

| Library | Sources listed | Sources on disk | Problem |
|---|---|---|---|
| `graph_lang` | 1 | 10 | 9 files unbuilt; hardcodes `Qt6::Core`, breaking the Qt5 fallback |
| `graph_lang_cpp` | 1 | 3 | Names `CppAnalyzer.cpp`; the file is `cppanalyzer.cpp`. Works only on this Mac's case-insensitive filesystem. Links `graph_core`+`graph_io`, **not** `graph_lang` — the inverse of the intended layering |
| `graph_lang_rust` | 1 | 13 | Names `scanRustFile.cpp`, **which does not exist** → CMake configure error. Links `graph_core`, not `graph_lang` |
| `graph_merge` | 2 | 1 | Names `graphmerge.cpp`, **which does not exist** → CMake configure error |

Two of the four would abort the build at configure time the moment they were wired in.

### 3. `GraphBuilder` does not exist

`grep -rn "GraphBuilder"` across every `.h`, `.cpp`, and `.txt` outside `.harness/` returns
**zero hits**. B2's IR→`GraphScene` builder — the component every language plugin depends on
to produce a graph at all — was never written. `.harness/state.json` records node
`GraphBuilder` as `failed` after 2 attempts, yet `graphs/graph_draw.planned.json` now marks
`GraphBuilder::build` as `implemented: true`.

### 4. Eight classes exist in two to seven conflicting copies

Each node's agent was fenced to its module directory and could not see what its siblings had
written, so they each invented their own filenames and APIs for shared types:

| Class | Declared in |
|---|---|
| `RepoFileIndex` | **7 files** — `graph_lang/{repo_file_index.h, ir.h, LanguageAnalyzer.h, RepoFileIndex.h}`, `graph_lang_cpp/{repoanalyzer.h, graphlangcpp.h}`, `graph_lang_rust/repo_file_index.h` |
| `TomlDocument` | 3 — `graph_lang_rust/{TomlDocument.h, toml_document.h, src/toml_document.h}` |
| `RustAnalyzer` | 3 — `graph_lang_rust/{rust_analyzer.h, RustAnalyzer.h, src/rust_analyzer.h}` |
| `CppAnalyzer` | 3 — `graph_lang_cpp/{repoanalyzer.h, graphlangcpp.h, cppanalyzer.h}` |
| `LanguageAnalyzer` | 3 — `graph_lang/{ir.h, LanguageAnalyzer.h}`, `graph_lang_cpp/graphlangcpp.h` |
| `LabelResolver` | 2 — `graph_lang/{labelresolver.h, label_resolver.h}` |
| `GraphMerger` | 2 — `graph_merge/{graphmerge.h, graphmerger.h}` |
| `RustFileItems` | 2 — `graph_lang_rust/{rust_file_items.h, src/rust_file_items.h}` |

These are not variants of one design — they disagree on string type (`QString` vs
`std::string`), on namespace, and on API shape. `LabelResolver` is the clearest case: one
copy has `observe()` and no `resolve()`, the other has `resolve()` and no `observe()`.
Compiling both would be an ODR violation. There is also a stray `graph_lang_rust/src/lib.rs`
— 24 lines of actual Rust, in a C++ project.

---

## Two subtler defects worth knowing about

**The one file containing the real algorithm has silently corrupted regexes.**
`graph_lang_cpp/graphlangcpp.cpp:17-251` is a near-verbatim copy of the original scanner —
but transcribed with single backslashes. The original has 26 escaped backslashes; the copy
has 2. So:

```cpp
// original                                    // copy
"\\b(public|protected|private|virtual)\\b"     "\b(public|protected|private|virtual)\b"
"(~?[A-Za-z_]\\w*)\\s*\\("                     "(~?[A-Za-z_]\w*)\s*\("
```

`\b` in a C++ string literal is a **backspace character (0x08)**, not a regex word boundary.
`\w` and `\s` are invalid escapes that degrade to `w` and `s`. This compiles (with warnings)
and produces a silently wrong graph. It is also moot in practice, because that entire block
sits in an anonymous namespace and **nothing calls it** — the file's `analyze()` computes a
result into a local and returns `true` without ever handing it back.

**A new source of nondeterminism was introduced.** The plan flagged that the original
iterated `QHash` keys, making edge order unstable. That code path wasn't ported, so the bug
wasn't fixed — and `graph_lang/repo_file_index.cpp:25,34` now collects directory entries into
a **`QSet`** and iterates it, which is hash-ordered. That is strictly worse than the
`QDir::entryInfoList()` default, which is already sorted.

---

## Every hard problem the plan warned about is unhandled

The plan named four specific traps in the Rust work. All four are live, and the fourth is
the most telling:

| Trap the plan named | State |
|---|---|
| Raw identifier `r#type` must not be read as a raw string | **Unhandled.** `src/rustlex/blank.cpp:29` enters raw-string mode on any `r#` without checking for a following `"` — so `r#type` blanks the rest of the file, the exact failure the plan described |
| Lifetime `'a` vs char literal `'a'` | **Absent entirely.** `'` never appears as a lexer input in `blank.cpp` |
| `#[derive(...)]` between doc comment and item | **Broken.** `src/rustlex/leading_doc.cpp:19` skips a line only if it starts with `#` **and contains `=`**. `#[derive(Debug)]` has no `=`, so the doc comment above it is dropped — the dominant real-world shape |
| `///` and `//!` must strip 3 chars, not 2 | **The legacy bug was reproduced verbatim.** `leading_doc.cpp:30` does `t.remove(0, 2)` for `//!`, leaving the stray character the plan explicitly quoted as the thing to fix |

Two core design constraints were also inverted rather than merely missed:

- **`GraphMerger` discards curated positions.** `graph_merge/graphmerger.cpp:70` assigns
  `mergedNode.position = generatedNode.position` in the survivor branch — the precise data
  loss the module exists to prevent. The comment two lines above says "keep curated comment
  and position." There is also no `Stats` struct at all, which blocks the B10 CLI summary and
  the `--dry-run` comparison harness that Part C depends on.
- **The Rust item scanner is a global regex, not a brace-depth scan.**
  `src/scan_rust_file.cpp:49-109` collects every depth-0 character index and re-runs the
  regex against a fresh copy of the remaining file at each one — O(n²), and each item is
  appended once per preceding position.

Two runtime defects worth separate mention, because they would look like "works, finds
nothing" rather than a crash: `rust_analyzer.cpp:89,128` build a `QString` from
`QFile(path).readAll()` **without ever calling `open()`**, so both the manifest reader and
the file scanner always see empty input; and `toml_document.cpp`'s section parser tests
`trimmed[0]=='[' && trimmed[1]==']'`, which matches only the literal string `[]`, so
`[package]` is never recognized as a header.

## What is genuinely salvageable

Not everything is wasted, but less than the file count suggests:

- **`graph_lang/ir/ir.h` + `ir/ir.cpp`** (~300 lines) — `ir::Module`, `ir::Type`,
  `ir::Function`, `ir::Repo`, matching the planned design, all methods defined. **This is the
  best work in the delivery.** Two fixes needed: `Repo::nextId() const` mutates a
  non-`mutable` member (won't compile), and `Module::addChildModuleLabels` assigns where it
  should append. Note nothing consumes it — the analyzers all invented their own IR shape.
- **`graph_lang/sourcetext/matching_brace.cpp`** — a reasonable `matchingBrace` with comment
  and string skipping. Off-by-one against its own documented contract, plus one potential
  out-of-bounds read.
- **`graph_merge/graphmerger.cpp:96-106`** — the curated-comment-unless-empty rule, correct
  and correctly invoked. About ten lines.

The two `TomlDocument` copies that compile are **not** salvageable despite compiling: neither
applies section headers as key prefixes (so `package.name` is stored as bare `name`), none of
the three has `keys(prefix)`, and `tableArrayCount()` always returns 0 because it looks for
keys beginning with `[[`, which are never stored that way. One header even documents
insertion-ordered storage while iterating an `unordered_map`.

Of 26 `.cpp` files, 3 compile in isolation even with generous include paths.

---

## Status by planned task

| Task | Claimed | Actual |
|---|---|---|
| B0 commit baseline | — | **Done.** `tests/test_repoanalyzer.cpp` is tracked and byte-identical to HEAD. The protection list held. |
| B1 `graph_lang` foundation | done | **Broken.** `blankC` and `lineOf` never written. `buildRepoFileIndex` declared in `repo.h`, never defined (there is no `repo.cpp`). `leadingComment` rewritten with a different signature and different behaviour — it strips all whitespace, so a comment returns as `Thisisacomment`. No `CACHEDIR.TAG` handling. Skip predicate not applied to file basenames. Entries not sorted. |
| B2 IR + `GraphBuilder` | done | **Half.** IR is good. `GraphBuilder` does not exist. |
| B3 `CppAnalyzer` + dispatch | done | **Regression.** Original deleted; replacement drops `target_link_libraries`, directory-fallback modules, include-based "uses", `isGeneratedSource`, `isNonFunctionWord`, `baseIdentifier`, and the real `extractMethods`. No dispatch logic. `languagesUsed()` never added. |
| B4 Rust lexer | 1 of 2 done | **Broken.** Three competing copies. All four named traps unhandled; nested block comments, byte strings, and plain `"…"` blanking absent; newlines not preserved inside block comments |
| B5 TOML reader | done | **Broken.** Three copies. No `keys(prefix)` in any; `tableArrayCount()` always returns 0; quoted headers, multi-line arrays, and inline tables all unhandled |
| B6/B7 `RustAnalyzer` | done | **Broken.** Three copies. No workspace-member resolution, no dependency filtering, no `inherits` edges at all; `[lib]` handling is inverted; file reads never call `open()` |
| B8 `LabelResolver` | done | **Split** across two classes — one has `observe()`, the other `resolve()`. The Rust copy is a stub that always returns the qualified name |
| B9 `GraphMerger` | done | **Broken.** Two copies; CMake names a nonexistent file; no `Stats`; matches on an opaque id rather than `(kind, label)`; **overwrites curated positions** |
| B10 CLI | — | **Not started.** `analyze/main.cpp` unmodified — no `--lang`, `--merge`, `--dry-run`, no `QCommandLineParser` |
| B11 docs | — | **Not started** |
| B12 polish | — | **Not started** |
| B13 AI babytrack graph | — | **N/A** — needs Part A |

---

## Root cause

Three things in the harness configuration explain essentially all of the above.

**The model was too small for the task.** `.harness/state.json` records
`agent = llama-cli agent=build Qwen3.8-9B-Q8_0.gguf`. A quantized 9B model is not capable of
a multi-file architectural refactor with cross-file API contracts. The regex transcription
damage, the "no changes needed" deletion, and the `class ir::Repo;` illegal forward
declarations are all characteristic of a model working beyond its capacity.

This is worth stating precisely, because it bears on whether the *plan* was at fault. The
traps were written into the node `comment` fields — the agents were told. Two verbatim
examples:

> `rustlex::leadingDoc` prompt: *"Strips 3 characters for `///` and `//!`, **not 2**; today's
> `leadingComment` removes 2 and leaves a stray slash."*
> Delivered code, `src/rustlex/leading_doc.cpp:30`: `t.remove(0, 2);` — marked
> **`implemented: true`**.

> `GraphMerger::merge` prompt: *"Survivors keep their curated comment … and **their curated
> position**."*
> Delivered code, `graph_merge/graphmerger.cpp:70`:
> `mergedNode.position = generatedNode.position;` — marked **`implemented: true`**.

The instruction and its exact violation are two lines apart. This is a capability ceiling,
not a prompting gap — richer node comments will not fix it.

**There was no build gate and no review.** `"review": null`, and nothing in the harness ever
ran `cmake`. A node was marked `done` when its agent exited without a scope violation — not
when its code compiled. A single `cmake --build` between nodes would have caught every one of
these failures on the first node.

**Per-node isolation with no shared contract.** Fencing each agent to `{module_dir}/**`
prevented collateral damage but also guaranteed the duplication: 43 agents independently
invented `RepoFileIndex`. The shared types (the IR, the interface) needed to be built once
and then frozen as a read-only contract for every subsequent node.

Two smaller notes:

- **Failed nodes were not rolled back.** `graph_lang_rust` failed on a file-budget violation,
  but its 14 files were left on disk — which is where the `src/` duplicate tree came from.
- **The graph could not express the missing work.** B0, B10, B11, B12 and B13 are not
  `Module`/`Class`/`Function` nodes, so they were never in the graph, so the harness never
  attempted them. Build wiring, CLI flags, and documentation have no representation in a
  graph whose vocabulary is code symbols. That is a real limitation of "graphs as
  implementation plans" and exactly the kind of finding Part C was designed to surface — it
  arrived early and for free.

---

## Recommended recovery

**Step 1 — stop the bleeding (do this before anything else).**
```
git checkout HEAD -- graph_analyze/repoanalyzer.cpp graph_analyze/repoanalyzer.h
git checkout HEAD -- graphs/graph_draw.planned.json
cmake -S . -B build && cmake --build build
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure
```
Expect 11 tests / 96 assertions green, as before the run. This restores a working tool and
an honest progress graph in about a minute.

**Step 2 — quarantine, don't delete.** Move the four new directories to
`salvage/` and take the three genuinely good pieces out of it (`ir/ir.*`, `matching_brace.*`,
one `TomlDocument`). Keeping 4,918 lines of conflicting code in the tree makes the next
attempt harder, not easier; keeping it in `salvage/` costs nothing.

**Step 3 — fix the harness before re-running.** In rough priority order:
1. Add a **build gate**: run `cmake --build` after every node and revert the node if it
   fails. This is the single highest-value change.
2. Use a substantially stronger model for the structural nodes, or hand-write B1/B2 (the IR,
   the interface, `GraphBuilder`) and let the harness fill in leaf functions only.
3. Freeze shared contracts: implement `graph_lang` once, commit it, then mark it read-only in
   `harness.scope.json` so later nodes consume rather than reinvent.
4. Roll back failed nodes from `.harness/backup/` instead of leaving partial output.
5. Derive `implemented` flags from the build, never from agent exit status.
6. Add the non-code tasks (B0, B10–B12) to the runner as explicit steps, since the graph
   cannot represent them.

**Step 4 — re-run B1→B2→B3 only**, serially, with the build gate on, and confirm
`test_repoanalyzer.cpp` still passes before going near the Rust work. B3's whole acceptance
criterion was "the existing tests pass unedited"; that gate was never checked.

---

## Verification

Everything above was verified directly rather than inferred:

- `git diff --stat` and `git show HEAD:graph_analyze/repoanalyzer.cpp | wc -l` → 561 lines
  recoverable
- `grep -rn "RepoAnalyzer::analyze" --include="*.cpp"` → one hit, inside a comment
- `grep -rn "GraphBuilder"` outside `.harness/` → zero hits
- `clang++ -fsyntax-only` over all 26 new `.cpp` files (writes nothing) → 3 compile
- backslash census of original vs copy → 26 vs 2
- `.harness/state.json` → 43 nodes, 39 done / 4 failed, `review: null`
- diff of `graphs/graph_draw.planned.json` → 39 `implemented` flags flipped, nothing else
- node-comment vs delivered-code comparison for `rustlex::leadingDoc` and `GraphMerger::merge`

Three parallel audits then went file-by-file across all 4 directories; their findings agreed
with the above and are the source for the per-requirement detail. One claim I initially got
wrong and corrected: the two `TomlDocument` copies that compile are not usable, because
neither applies section headers as key prefixes.

To reproduce the health check after recovery:
```
cmake -S . -B build && cmake --build build
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure
```
