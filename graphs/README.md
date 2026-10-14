# Graphs

Saved graphs in the `graph_io` JSON format. The focus of this repo is the
graphs themselves: each node carries the prompt/description used to implement
the code it represents, and the graph tracks whether each dependency has been
implemented.

## Node fields

| Field         | Meaning                                                              |
|---------------|---------------------------------------------------------------------|
| `kind`        | `Module`, `Class`, or `Function`.                                    |
| `name`        | The node label.                                                      |
| `comment`     | The **implementation prompt/description** for this node.             |
| `implemented` | Whether the code this node represents has actually been built.       |
| `position`    | `{x, y}` scene coordinates.                                          |

`implemented` is optional on load and defaults to `false`, so older graphs
still load.

## Dependency convention

Edges are directed. **`A → B` (arrow pointing at B) means "A depends on B".**
So a node's dependencies are the targets of its outgoing edges. The model
exposes this on `GraphNode`:

- `dependencies()` / `dependents()`
- `unimplementedDependencies()` — the pieces that must be built (or stubbed with
  a `NotImplementedError`) before this node can be implemented against real code
- `dependencyStatus()` — `NoDependencies`, `Ready`, or `Blocked`

and graph-wide on `GraphScene`:

- `readyToImplement()` — unimplemented nodes whose dependencies are all done
- `blocked()` — unimplemented nodes still waiting on a dependency

## Files

- `graph_draw.json` — a hand-curated self-describing graph of this repository:
  its modules, classes, and key functions, with each `comment` holding a written
  implementation prompt. All nodes are marked implemented because the code
  exists today.
- `graph_draw.generated.json` — the same repository as produced automatically by
  the `repo_to_graph` tool (see below). Broader coverage (every class and member
  function) but machine-written descriptions.
- `graph_draw.planned.json` — this repository as it will stand **after** the
  language-plugin refactor described in `../IMPLEMENTATION_PLAN.md` (Part B). The
  24 nodes that exist today are `implemented: true`; the 43 the refactor creates
  are `false`, so `readyToImplement()` and `blocked()` act as the work queue.
  Each unimplemented node's `comment` is a real implementation prompt carrying the
  specific traps for that piece.

Do not confuse the three: `graph_draw.json` is the curated present,
`graph_draw.generated.json` the machine-scanned present, `graph_draw.planned.json`
the intended future. Only the last has unimplemented nodes.

## Generating a graph from a repo

The `repo_to_graph` executable scans a C++ or Rust repo and emits a graph in
this format:

```
repo_to_graph [options] repo-root [output.json]

Options:
  --lang <id>              Restrict analysis to one language id (cpp, rust).
                           Default: autodetect.
  --merge <existing.json>  Merge the freshly analyzed graph into this existing
                           curated graph instead of overwriting the output path
                           from scratch.
  --dry-run                With --merge, report kept/added/stale counts without
                           writing any file.
```

With no output path it writes `<repo-root>/graphs/<repo-name>.generated.json`
(the `.generated` suffix means it never overwrites a curated `<name>.json`,
and `--merge`'s input is never clobbered either). It is a self-contained
heuristic scanner — no libclang, no rustc:

- **C++**: modules come from CMake `add_library`/`add_executable` targets,
  classes from `class`/`struct` definitions, and functions from their member
  functions.
- **Rust**: modules come from Cargo crates (`[workspace]` members plus every
  `Cargo.toml`, deduped by directory) with depth-1 nested modules derived from
  `src/<name>.rs`/`src/<name>/`; `dependsOnLabels`-style edges come from
  `[dependencies]`/`[dev-dependencies]`/`[build-dependencies]` resolved against
  other discovered crates; classes come from `struct`/`enum`/`trait`
  definitions, with `impl Trait for Type` producing an inherits edge; functions
  come from methods and free functions; `use`-resolved references to other
  repo-defined types produce "uses" edges. Two crates (or a crate and its own
  nested module) sharing a type or function name are disambiguated into
  separate, correctly-owned nodes rather than one silently dropping the other.

A polyglot repo (no `--lang`) runs every applicable analyzer into one shared
graph. Every discovered node is marked implemented, since it reflects code
that already exists. See `graph_analyze/repoanalyzer.h` for the edge
conventions and known limitations, and `../RUST_GRAPH_PLAN.md` for the Rust
support effort's status and remaining known limitations.
