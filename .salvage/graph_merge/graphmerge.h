#pragma once
#include "graph_core/graphscene.h"
#include <QList>
#include <QString>
#include <QMap>

// ── graph_merge module ────────────────────────────────────────────────────────
//
// Folds a freshly generated graph into a hand-curated plan without destroying
// curated prose or layout. Depends on graph_core only, deliberately not
// graph_io, so it is testable with two in-memory scenes.
//
// The curated graph is authoritative for prose and layout; the generated
// graph is authoritative for what exists on disk. Nothing is ever deleted:
// nodes missing from the generated graph are added as stubs.

class GraphMerger;

// ── GraphMerge ───────────────────────────────────────────────────────────────

/**
 * @brief Merges a generated graph into a curated plan.
 *
 * The curated graph provides the authoritative prose and layout.
 * The generated graph provides the authoritative "exists on disk" state.
 *
 * Matching is done by (kind, label) pair. Nodes missing from the generated
 * graph are added as stubs. Edges are merged similarly.
 *
 * @param curated  The hand-curated plan (authoritative for prose/layout).
 * @param generated The freshly generated graph (authoritative for disk existence).
 * @return A new merged scene containing all nodes and edges from both graphs.
 */
Q_DECLARE_METATYPE(GraphMerger)

class GraphMerger {
public:
    /**
     * @brief Constructs a merger for the given curated scene.
     *
     * @param curated The curated scene that defines prose and layout.
     */
    explicit GraphMerger(GraphScene* curated);

    /**
     * @brief Merges the generated scene into the curated one.
     *
     * Matching is done by (kind, label) pair. Nodes missing from the
     * generated graph are added as stubs. Edges are merged similarly.
     *
     * @param generated The generated scene (authoritative for disk existence).
     * @return A new merged scene.
     */
    GraphScene* merge(GraphScene* generated) const;

private:
    GraphScene* m_curated;

    // Find or create a node in the curated scene by (kind, label).
    GraphNode* findOrCreateNode(NodeType kind, const QString& label) const;

    // Merge edges: curated edges are authoritative for label,
    // generated edges are authoritative for existence.
    void mergeEdges(GraphScene* generated) const;

    // Add a stub node for a node that exists in generated but not in curated.
    void addStub(GraphNode* generatedNode);
};
