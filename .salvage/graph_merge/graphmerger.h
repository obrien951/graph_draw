#ifndef GRAPH_MERGE_GRAPHMERGER_H
#define GRAPH_MERGE_GRAPHMERGER_H

#include "graph_core/graphscene.h"
#include "graph_core/graphedge.h"
#include "graph_core/graphnode.h"
#include <vector>
#include <string>
#include <memory>

namespace graph_merge {

/**
 * @brief Merges a generated graph into a curated graph.
 * 
 * The curated graph is authoritative for prose and layout.
 * The generated graph is authoritative for what exists on disk.
 * Nothing is ever deleted from the curated graph.
 */
class GraphMerger {
public:
    /**
     * @brief Merges a generated graph into a curated graph.
     * 
     * Survivors (nodes in both graphs) keep their curated comment
     * (unless empty, in which case it is filled from the generated one)
     * and their curated position.
     * 
     * New nodes are laid out BELOW the curated bounding box.
     * Curated-only nodes are kept and forced to implemented=false.
     * Edges are purely additive.
     * 
     * @param curated The curated (authoritative) graph.
     * @param generated The generated (disk-based) graph.
     * @return The merged graph.
     */
    static GraphScene merge(const GraphScene& curated, const GraphScene& generated);

    /**
     * @brief Merges a generated graph into a curated graph.
     * 
     * @param curated The curated (authoritative) graph.
     * @param generated The generated (disk-based) graph.
     * @param layoutStrategy Strategy for placing new nodes.
     * @return The merged graph.
     */
    static GraphScene merge(
        const GraphScene& curated,
        const GraphScene& generated,
        LayoutStrategy layoutStrategy = LayoutStrategy::BELOW_BOUNDING_BOX
    );

private:
    enum class LayoutStrategy {
        BELOW_BOUNDING_BOX,
        APPEND,
        AUTO
    };

    static GraphScene::NodeMap mergeNodes(const GraphScene::NodeMap& curatedNodes,
                                           const GraphScene::NodeMap& generatedNodes);

    static GraphScene::EdgeMap mergeEdges(const GraphScene::EdgeMap& curatedEdges,
                                           const GraphScene::EdgeMap& generatedEdges);

    static std::string mergeComment(const std::string& curatedComment,
                                     const std::string& generatedComment);

    static bool isCuratedOnly(const GraphScene& curated, const GraphScene& generated);
};

} // namespace graph_merge

#endif // GRAPH_MERGE_GRAPHMERGER_H
