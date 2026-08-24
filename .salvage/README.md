# Quarantined output of the 2026-08-19 harness run

This directory holds the four libraries produced by the `agent_harness` run over
`graphs/graph_draw.planned.json` (Part B of `IMPLEMENTATION_PLAN.md`). Full analysis is in
`../PART_B_STATUS.md`.

**Nothing here was deleted.** It was moved out of the repo root because 4,918 lines of
mutually-conflicting code (eight classes defined 2–7 times each, with incompatible APIs)
makes the next attempt harder, not easier.

## Why the name starts with a dot

`RepoAnalyzer::isSkippedDir` (`graph_analyze/repoanalyzer.cpp:44`) skips any directory whose
name begins with `.`. Naming this `.salvage/` therefore keeps it out of every graph
`repo_to_graph` generates from now on, with no change to the analyzer. A plain `salvage/`
would be scanned and would pollute the output.

## What is in here

| Directory | Lines | State |
|---|---|---|
| `graph_lang/` | ~1,150 | Foundation. IR is good (see KEEP); everything else is duplicated or broken |
| `graph_lang_cpp/` | ~840 | Three competing `CppAnalyzer` classes. Contains the only copy of the original scanner algorithm, with corrupted regex escapes |
| `graph_lang_rust/` | ~2,200 | Three competing copies of every component, in three naming conventions |
| `graph_merge/` | ~300 | Two competing `GraphMerger` classes; neither compiles |
| `KEEP/` | ~450 | The three pieces worth carrying forward — see below |

## KEEP/ — the parts worth reusing

Copied here verbatim; original paths noted. Each still needs the listed fix.

**`ir.h` / `ir.cpp`** — from `graph_lang/ir/`. `ir::Module`, `ir::Type`, `ir::Function`,
`ir::Repo`, matching the planned design with all methods defined. The best work in the run.
- `Repo::nextId() const` mutates a non-`mutable` member — will not compile. Make `m_nextId`
  mutable or drop the `const`.
- `Module::addChildModuleLabels` assigns where the name implies append.
- Nothing consumed it: every analyzer invented its own IR shape instead.

**`matching_brace.h` / `matching_brace.cpp`** — from `graph_lang/sourcetext/`. Brace matching
with comment and string skipping.
- Returns the index *of* the closing `}`, while its own header documents "the index
  immediately after". Pick one.
- `pos` can advance past `source.size()` before a dereference — possible out-of-bounds read.

**`mergeComment.snippet.cpp`** — from `graph_merge/graphmerger.cpp:96-106`. The
curated-comment-unless-empty rule, correct and correctly invoked. Roughly ten lines, but it
is the one merge requirement that was implemented as specified.

## What is NOT worth keeping, despite compiling

The two `TomlDocument` copies that compile are unusable: neither applies section headers as
key prefixes (so `package.name` is stored as bare `name`), none of the three copies has
`keys(prefix)`, and `tableArrayCount()` always returns 0 because it looks for keys beginning
with `[[`, which are never stored that way.
