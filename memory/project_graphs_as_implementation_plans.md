---
name: Graphs as implementation plans
description: Repo focus shifted from visualization to graphs that encode implementation prompts and track dependency completion
metadata:
  type: project
---

As of 2026-07-26 the repo's focus moved away from the visualization side toward
the graphs themselves. A graph node now represents a module/class/function whose
`comment` field doubles as the **implementation prompt/description** used to
build it, plus an `implemented` flag.

Dependency tracking convention: an edge `A → B` means "A depends on B". The
model exposes `GraphNode::dependencies/dependents/unimplementedDependencies/
dependencyStatus` and `GraphScene::readyToImplement/blocked`. The point is to let
an agent decide, per dependency, whether to implement against real code or stub
it with a `NotImplementedError`.

In the app, double-clicking a node (Select mode) opens an edit dialog with a
multi-line **Description** editor (the prompt, stored in `comment`) and an
**Implemented** checkbox. Each node paints a status badge: green check when
implemented, dashed ring when not.

`graphs/graph_draw.json` is a hand-curated self-describing graph of this repo
(see `graphs/README.md`), loadable via the app's Load action. Reuse the existing
`comment` field for prompts — the user chose not to add a separate `prompt`
field. See [[feedback_project_structure]].

`graph_analyze` library + `repo_to_graph` executable auto-generate a graph from
any C++ repo: `repo_to_graph <repo-root> [out.json]`. Self-contained heuristic
scanner (no libclang) — modules from CMake add_library/add_executable targets,
classes from class/struct defs, functions from member functions. Default output
is `graphs/<name>.generated.json` (the `.generated` suffix avoids clobbering
curated graphs — it once overwrote the curated graph_draw.json, hence the rule).
