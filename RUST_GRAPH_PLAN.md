# Rust graph support — status audit + remaining plan

Resume: `claude --resume debecc96-6d9f-404b-90d4-3b3159838bca`

## Status (2026-10-14): R1–R6 complete

All six tasks below are done, verified by independent build+test runs and
real end-to-end smoke tests against synthetic Cargo workspaces, not just by
the dispatched agents' own say-so. `repo_to_graph --lang rust <repo> [out]`
works, including `--merge`/`--dry-run`. One task (R4.5) was added mid-stream
when R4 surfaced a correctness bug upstream of it.

**One known remaining limitation:** two different crates each having a child
module with the same name (e.g. both with `src/config.rs`) still collide into
one node, because `ir::Module` has no field recording a child module's owning
crate the way `Type::moduleLabel` does for types. Fixing it needs a new IR
field touching every analyzer (C++ and Rust both), which was deliberately left
out of this effort's scope.

**Bottom line up front:** almost all the hard parsing work for Rust already
exists and passes its tests. The thing standing between you and a working
Rust graph is that `RustAnalyzer` is never actually registered with the
dispatcher, and it stops after crate discovery instead of calling the file
scanner that already exists. This is a wiring gap, not a parsing gap.

## Why this plan exists instead of just reusing `IMPLEMENTATION_PLAN.md`

`IMPLEMENTATION_PLAN.md` (tasks B0–B12) is the original design doc for
polyglot support and is still the right reference for *intent and edge
cases* (raw identifiers, lifetime-vs-char lexing, TOML quirks, etc.) — don't
redo that thinking. But `.harness/state.json` marks `RustAnalyzer`,
`RustAnalyzer::analyze`, and `scanRustFile` as `done`, and that's only true
in the narrow sense that the functions exist and compile. Verified today
(2026-10-14) by reading the actual source and running the suite:

- **Build is clean, 110/110 tests pass** (`cmake --build build -j4`,
  `ctest --test-dir build`).
- `graph_lang_rust/rustlex.h` / `stubs.cpp` (B4, lexer) — real, well-tested
  (`tests/test_rustlexer.cpp`, 436 lines covering raw strings, raw
  identifiers, nested comments, lifetime/char ambiguity).
- `graph_lang_rust/toml_document.cpp` (B5, TOML reader) — real, tested
  (`tests/test_cargotoml.cpp`).
- `graph_lang_rust/rust_file_items.h` + `scanRustFile` in `stubs.cpp` (part
  of B7) — real: it already returns per-file `types` (with methods), free
  `functions`, and `usePaths`. **Nothing in the codebase calls it.**
- `graph_lang_rust/rust_analyzer.cpp` (`RustAnalyzer::analyze`, B6) — only
  does crate discovery from `Cargo.toml` → one `ir::Module` per crate with a
  `label`/`description`. It never sets `dependsOnLabels`, `childModuleLabels`,
  or anything on `ir::Type` — so even if it ran, it would currently produce
  crate nodes with no edges and no types.
- `tests/test_rustanalyzer.cpp` is **8 lines** — it only checks
  `RustAnalyzer().id() == "rust"`. There is no test that `detect()` or
  `analyze()` produce correct crates, modules, edges, or types. The B6
  acceptance criterion ("a `buildSampleCargoWorkspace` fixture") was never
  written.
- `graph_analyze/repoanalyzer.cpp:30` — the dispatcher's analyzer list is
  `QVector<LanguageAnalyzer*> available = { &cpp };`. **`RustAnalyzer` is not
  in it.** Point `repo_to_graph` at a Rust repo today and you get "No
  supported source files found", full stop.
- B9 (`GraphMerger`, `graph_merge/graphmerger.cpp`) is real and tested.
- B10 (CLI flags `--lang`/`--merge`/`--dry-run`) was never done —
  `analyze/main.cpp` is still `repo_to_graph <repo-root> [output.json]`
  with no `QCommandLineParser`, no language selection, no merge support.

So: lexer ✅, TOML ✅, file-level item scanning ✅, merge ✅ — but the piece
that turns those into a Rust `ir::Repo` (B6/B7 proper) and the piece that
lets you ask for Rust from the command line (B10) are the gap.

## Remaining tasks, in the order to do them

**R1 — Register `RustAnalyzer` with the dispatcher** · ~15 min
`graph_analyze/repoanalyzer.cpp:30`: add `#include "rust_analyzer.h"`,
instantiate `RustAnalyzer rust;`, add `&rust` to `available`. Link
`graph_lang_rust` into `graph_analyze` (`graph_analyze/CMakeLists.txt`).
This alone makes `--lang rust` (once R5 adds the flag) or autodetect
(`Cargo.toml` present, no CMake) produce *something* — crate nodes, still
with no edges or types until R2/R3 land.

**R2 — Finish `RustAnalyzer::analyze`: nested modules + crate deps** ·
~3–4 h (per B6 in `IMPLEMENTATION_PLAN.md:460`)
In `graph_lang_rust/rust_analyzer.cpp`, after the existing crate-module
loop:
- For each crate dir, walk `src/` one level: each `src/<name>.rs` or
  `src/<name>/` becomes a child `ir::Module` with a crate→module
  `childModuleLabels` entry (depth-1 only, no `mod` resolution needed).
- Read `dependencies`/`dev-dependencies`/`build-dependencies` (plus
  `[workspace.dependencies]`) via the already-working `TomlDocument::keys`,
  normalize `-`↔`_`, and set `dependsOnLabels` on the crate module for every
  name that matches another discovered crate.
- Write the `buildSampleCargoWorkspace` test fixture the original plan
  called for (mirror `tests/test_repoanalyzer.cpp`'s
  `buildSampleRepo`), including a decoy `target/` with `CACHEDIR.TAG`
  (already handled by `RepoFileIndex`, per B1) to prove it's excluded.

**R3 — Wire `scanRustFile` into `RustAnalyzer::analyze`: types/methods/uses**
· ~2–3 h (cheaper than B7's original 6–8h estimate because the scanner
itself is already written and tested — this is call-site + IR-mapping work)
For every `.rs` file under each crate's `src/`: call `scanRustFile`, map
each `RustFileItems::Type` → `ir::Type` (label, methods from
`Type::methods`, `moduleLabel` = owning crate/module), map free `functions`
into the owning module's `ir::Module::functions`. Resolve `usePaths`
against the set of repo-defined type labels (file-granular, matching the
existing C++ "uses" heuristic's fidelity) to populate `usesLabels`;
anything not resolving to a repo type (e.g. `serde`, `axum`, `std::...`)
produces nothing, same rule as B7. Add the real tests B7 asked for: trait
with required+provided methods, `impl Trait for Type` → inherits edge,
`#[derive]` sitting between a doc comment and the item, a nested `fn`
inside a function body that must **not** appear as a top-level node.

**R4 — `LabelResolver` sanity check** · ~30 min
Confirm `graph_lang/labelresolver.cpp` is actually invoked somewhere in the
build path for cross-crate name collisions (e.g. two crates each defining
`Error`/`Config`). If it's currently only wired for C++, extend the call
site to run over the merged IR regardless of language — this was supposed
to be language-neutral per B8's design.

**R5 — CLI flags on `repo_to_graph`** · ~2 h (B10, `IMPLEMENTATION_PLAN.md:514`)
Rewrite `analyze/main.cpp` with `QCommandLineParser` for free `--help`:
- `--lang cpp|rust` → forwarded to `RepoAnalyzer::setLanguage`; omitted
  means autodetect (and runs both analyzers on a polyglot tree, first-wins
  dedup, per B10).
- `--merge <existing.json>` → load the existing graph, run analysis fresh,
  call `GraphMerger::merge`, write back, print `kept N · added M · stale K`.
  Default output path stays `<root>/graphs/<name>.generated.json`; `--merge`
  must never silently write over its own input (there's a prior incident on
  record — see `memory/project_graphs_as_implementation_plans.md` — where
  this tool overwrote a hand-curated graph).
- `--dry-run` → run the merge comparison and print stats without writing.

**R6 — Docs** · ~30 min
Update `graph_analyze/repoanalyzer.h:6-24` and `analyze/main.cpp`'s own
usage text (both currently describe a C++-only tool), `graphs/README.md`,
and correct `.harness/state.json`'s stale `done` markings for
`RustAnalyzer`/`RustAnalyzer::analyze`/`scanRustFile` so the harness's own
bookkeeping stops claiming finished work that isn't — or at minimum don't
trust those three entries without reading the diff next time.

Total remaining: **roughly 9–11 hours**, almost entirely in R2/R3/R5.
R1 is the 15-minute fix that turns "nothing happens" into "something
happens," so do it first even if R2/R3 aren't ready — it'll immediately
surface whether crate discovery on a real workspace looks right before you
sink time into types/methods.

## The final executable call

Once R1–R5 land, from this repo:

```sh
# build it
cmake --build build -j4

# generate a graph for a Rust repo (autodetects Cargo.toml; QT_QPA_PLATFORM
# is only needed because GraphNode is a QGraphicsObject even in CLI mode)
QT_QPA_PLATFORM=offscreen ./build/repo_to_graph --lang rust \
    /path/to/your/rust/repo \
    graphs/your-repo.generated.json

# or, to enrich a hand-curated graph instead of overwriting it:
QT_QPA_PLATFORM=offscreen ./build/repo_to_graph --lang rust \
    --merge graphs/your-repo.json \
    /path/to/your/rust/repo \
    graphs/your-repo.generated.json
```

**Interpreting** the result is the existing `graph_draw` GUI (`Load` action
on the output JSON) — it already understands `implemented`/stub status and
exposes `GraphScene::readyToImplement()`/`blocked()` regardless of which
language produced the nodes, since the IR→scene step (`GraphBuilder`) is
language-neutral. No Rust-specific work is needed there.

Right now (before R1), the only true statement about what you can run is
`./build/repo_to_graph <repo-root>` against a **C++** repo — pointing it at
a Rust repo fails with "No supported source files found."
