#include <catch2/catch_all.hpp>
#include "graphscene.h"
#include "graphnode.h"

TEST_CASE("Last node added to a scene has the expected kind", "[graphscene][graphnode]")
{
    GraphScene scene;

    // Arrange: pre-existing node of a different type so the scene is not empty
    auto* moduleNode = new GraphNode(NodeType::Module, "CoreUtils");
    scene.addItem(moduleNode);

    // Act: add a Function node after the Module
    auto* fnNode = new GraphNode(NodeType::Function, "compute");
    scene.addItem(fnNode);

    // Collect every GraphNode in the scene
    QList<GraphNode*> nodes;
    for (QGraphicsItem* item : scene.items())
        if (auto* n = qgraphicsitem_cast<GraphNode*>(item))
            nodes.append(n);

    // scene.items() returns in front-to-back stacking order; the Function node
    // was added last so it sits in front and appears first in the list
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes.first()->kind() == NodeType::Function);
}
