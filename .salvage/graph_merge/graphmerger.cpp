#include "graph_merge/graphmerger.h"
#include <algorithm>
#include <stdexcept>

namespace graph_merge {

GraphScene GraphMerger::merge(const GraphScene& curated, const GraphScene& generated) {
    return merge(curated, generated, LayoutStrategy::BELOW_BOUNDING_BOX);
}

GraphScene GraphMerger::merge(
    const GraphScene& curated,
    const GraphScene& generated,
    LayoutStrategy layoutStrategy
) {
    GraphScene merged;

    // Merge nodes
    auto mergedNodes = mergeNodes(curated.nodes(), generated.nodes());

    // Merge edges (purely additive)
    auto mergedEdges = mergeEdges(curated.edges(), generated.edges());

    // Build the merged graph
    for (const auto& [id, node] : mergedNodes) {
        merged.addNode(node);
    }

    for (const auto& edge : mergedEdges) {
        merged.addEdge(edge);
    }

    // Apply layout strategy for new nodes
    if (layoutStrategy == LayoutStrategy::BELOW_BOUNDING_BOX) {
        applyBelowBoundingBoxLayout(merged, curated, generated);
    } else if (layoutStrategy == LayoutStrategy::APPEND) {
        applyAppendLayout(merged, generated);
    }

    return merged;
}

GraphScene::NodeMap GraphMerger::mergeNodes(
    const GraphScene::NodeMap& curatedNodes,
    const GraphScene::NodeMap& generatedNodes
) {
    GraphScene::NodeMap mergedNodes;

    // First, add all curated nodes (they are authoritative)
    for (const auto& [id, node] : curatedNodes) {
        mergedNodes[id] = node;
    }

    // Then, process generated nodes
    for (const auto& [id, generatedNode] : generatedNodes) {
        if (mergedNodes.find(id) == mergedNodes.end()) {
            // New node: create it below the curated bounding box
            mergedNodes[id] = generatedNode;
            mergedNodes[id].position = Position{
                .x = curatedNodes.empty() ? 0 : curatedNodes.begin()->second.position.x,
                .y = curatedNodes.empty() ? 0 : curatedNodes.begin()->second.position.y + 100
            };
        } else {
            // Survivor: keep curated comment and position
            auto& mergedNode = mergedNodes[id];
            mergedNode.comment = mergeComment(
                mergedNode.comment,
                generatedNode.comment
            );
            mergedNode.position = generatedNode.position;
        }
    }

    return mergedNodes;
}

GraphScene::EdgeMap GraphMerger::mergeEdges(
    const GraphScene::EdgeMap& curatedEdges,
    const GraphScene::EdgeMap& generatedEdges
) {
    GraphScene::EdgeMap mergedEdges;

    // Start with curated edges
    mergedEdges.insert(curatedEdges.begin(), curatedEdges.end());

    // Add generated edges that don't already exist (purely additive)
    for (const auto& edge : generatedEdges) {
        if (mergedEdges.find(edge) == mergedEdges.end()) {
            mergedEdges[edge] = edge;
        }
    }

    return mergedEdges;
}

std::string GraphMerger::mergeComment(
    const std::string& curatedComment,
    const std::string& generatedComment
) {
    // If curated comment is empty, use the generated one
    if (curatedComment.empty()) {
        return generatedComment;
    }
    // Otherwise, keep the curated comment (it's authoritative)
    return curatedComment;
}

bool GraphMerger::isCuratedOnly(const GraphScene& curated, const GraphScene& generated) {
    for (const auto& [id, node] : curated.nodes()) {
        if (generated.nodes().find(id) == generated.nodes().end()) {
            return true;
        }
    }
    return false;
}

void GraphMerger::applyBelowBoundingBoxLayout(GraphScene& merged,
                                              const GraphScene& curated,
                                              const GraphScene& generated) {
    // Find the bounding box of curated nodes
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;

    for (const auto& [id, node] : curated.nodes()) {
        minX = std::min(minX, node.position.x);
        minY = std::min(minY, node.position.y);
        maxX = std::max(maxX, node.position.x);
        maxY = std::max(maxY, node.position.y);
    }

    // Place new nodes below the bounding box
    int newY = maxY + 100;
    int newX = minX;

    for (auto& [id, node] : merged.nodes()) {
        // Only reposition nodes that are new (not in curated)
        if (curated.nodes().find(id) == curated.nodes().end()) {
            node.position.y = newY;
            node.position.x = newX;
            newX += 200; // Space out new nodes horizontally
        }
    }
}

void GraphMerger::applyAppendLayout(GraphScene& merged, const GraphScene& generated) {
    int y = 0;
    for (auto& [id, node] : merged.nodes()) {
        if (merged.nodes().find(id) == merged.nodes().end()) {
            node.position.y = y;
            node.position.x = 0;
            y += 100;
        }
    }
}

} // namespace graph_merge
