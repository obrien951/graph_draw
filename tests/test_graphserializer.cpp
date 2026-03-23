#include <catch2/catch_all.hpp>
#include "graphscene.h"
#include "graphnode.h"
#include "graphedge.h"
#include "graphserializer.h"
#include <QTemporaryFile>

// ── Helpers ───────────────────────────────────────────────────────────────────

static QList<GraphNode*> nodesIn(GraphScene& scene)
{
    QList<GraphNode*> result;
    for (QGraphicsItem* item : scene.items())
        if (auto* n = qgraphicsitem_cast<GraphNode*>(item))
            result.append(n);
    return result;
}

static QList<GraphEdge*> edgesIn(GraphScene& scene)
{
    QList<GraphEdge*> result;
    for (QGraphicsItem* item : scene.items())
        if (auto* e = qgraphicsitem_cast<GraphEdge*>(item))
            result.append(e);
    return result;
}

static GraphNode* findNode(GraphScene& scene, const QString& label)
{
    for (GraphNode* n : nodesIn(scene))
        if (n->label() == label) return n;
    return nullptr;
}

// ── Test ──────────────────────────────────────────────────────────────────────

TEST_CASE("Graph round-trips through a JSON file identically", "[graphserializer]")
{
    // ── Arrange: build the original scene ────────────────────────────────────
    GraphScene original;

    auto* moduleNode = new GraphNode(NodeType::Module, "CoreUtils");
    moduleNode->setPos(50.0, 120.0);
    original.addItem(moduleNode);

    auto* classNode = new GraphNode(NodeType::Class, "Parser");
    classNode->setPos(300.0, 80.0);
    original.addItem(classNode);

    auto* edge = new GraphEdge(moduleNode, classNode, "depends on");
    original.addItem(edge);

    // ── Act: save then load ──────────────────────────────────────────────────
    QTemporaryFile tmpFile;
    REQUIRE(tmpFile.open());
    const QString path = tmpFile.fileName();
    tmpFile.close();   // close so the serializer can open it for writing

    GraphSerializer serializer;
    REQUIRE(serializer.saveToFile(path, &original));

    GraphScene loaded;
    REQUIRE(serializer.loadFromFile(path, &loaded));

    // ── Assert: nodes are identical ──────────────────────────────────────────
    REQUIRE(nodesIn(loaded).size() == 2);

    GraphNode* loadedModule = findNode(loaded, "CoreUtils");
    REQUIRE(loadedModule != nullptr);
    REQUIRE(loadedModule->kind() == NodeType::Module);
    REQUIRE(loadedModule->pos().x() == Catch::Approx(50.0));
    REQUIRE(loadedModule->pos().y() == Catch::Approx(120.0));

    GraphNode* loadedClass = findNode(loaded, "Parser");
    REQUIRE(loadedClass != nullptr);
    REQUIRE(loadedClass->kind() == NodeType::Class);
    REQUIRE(loadedClass->pos().x() == Catch::Approx(300.0));
    REQUIRE(loadedClass->pos().y() == Catch::Approx(80.0));

    // ── Assert: edge is identical ────────────────────────────────────────────
    const QList<GraphEdge*> loadedEdges = edgesIn(loaded);
    REQUIRE(loadedEdges.size() == 1);

    GraphEdge* loadedEdge = loadedEdges.first();
    REQUIRE(loadedEdge->label()            == "depends on");
    REQUIRE(loadedEdge->source()->label()  == "CoreUtils");
    REQUIRE(loadedEdge->source()->kind()   == NodeType::Module);
    REQUIRE(loadedEdge->target()->label()  == "Parser");
    REQUIRE(loadedEdge->target()->kind()   == NodeType::Class);
}
