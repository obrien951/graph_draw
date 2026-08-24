#pragma once
#include <QString>

class GraphScene;
namespace ir { class Repo; }

// Turns an ir::Repo into scene items (B2).
//
// This is the SINGLE place that knows the five edge labels and the node layout.
// Language plugins never touch graph_core: they describe what they found in the
// IR, and this class decides how that becomes GraphNodes and GraphEdges. That is
// what lets graph_lang link Qt Core only and lets analyzer tests run without a
// QApplication.
//
// All edge endpoints in the IR are LABELS. A label that names no node is
// silently dropped, which reproduces the original scanner's "only link a base
// class that is itself a node" guard without special-casing it.
class GraphBuilder {
public:
    // Clear scene and rebuild it from repo. Creates modules, then types, then
    // functions — that order is also the JSON array order, so it must stay
    // deterministic.
    //
    // Emits seven edge categories over five labels:
    //   module -> module    "contains"     (nested modules)
    //   module -> class     "contains"
    //   module -> function  "contains"     (free functions; Rust reuses the label)
    //   class  -> function  "provides"
    //   class  -> class     "inherits"
    //   class  -> class     "uses"
    //   module -> module    "depends on"
    void build(const ir::Repo& repo, GraphScene* scene) const;
};
