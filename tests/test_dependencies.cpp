#include <catch2/catch_all.hpp>
#include "graphscene.h"
#include "graphnode.h"
#include "graphedge.h"

// Edge-direction convention under test: an edge A → B means "A depends on B".
// A node's dependencies are therefore the targets of its outgoing edges, and a
// node is "ready to implement" once all of its dependencies are implemented.

TEST_CASE("A node with no edges has no dependencies", "[dependencies]")
{
    GraphScene scene;
    auto* leaf = new GraphNode(NodeType::Function, "helper");
    scene.addItem(leaf);

    REQUIRE(leaf->dependencies().isEmpty());
    REQUIRE(leaf->dependents().isEmpty());
    REQUIRE(leaf->dependencyStatus() == DependencyStatus::NoDependencies);
    REQUIRE(leaf->unimplementedDependencies().isEmpty());
}

TEST_CASE("Dependencies follow edge direction (source depends on target)",
          "[dependencies]")
{
    GraphScene scene;
    auto* app  = new GraphNode(NodeType::Module, "app");
    auto* core = new GraphNode(NodeType::Module, "core");
    scene.addItem(app);
    scene.addItem(core);
    scene.addItem(new GraphEdge(app, core, "depends on"));   // app → core

    REQUIRE(app->dependencies().size() == 1);
    REQUIRE(app->dependencies().first() == core);
    REQUIRE(core->dependencies().isEmpty());

    REQUIRE(core->dependents().size() == 1);
    REQUIRE(core->dependents().first() == app);
}

TEST_CASE("A node is blocked until every dependency is implemented",
          "[dependencies]")
{
    GraphScene scene;
    auto* app  = new GraphNode(NodeType::Module, "app");
    auto* core = new GraphNode(NodeType::Module, "core");
    auto* io   = new GraphNode(NodeType::Module, "io");
    scene.addItem(app);
    scene.addItem(core);
    scene.addItem(io);
    scene.addItem(new GraphEdge(app, core));   // app depends on core
    scene.addItem(new GraphEdge(app, io));     // app depends on io

    // Nothing implemented yet → app is blocked by both dependencies.
    REQUIRE(app->dependencyStatus() == DependencyStatus::Blocked);
    REQUIRE(app->unimplementedDependencies().size() == 2);

    // Implement one dependency → still blocked by the other.
    core->setImplemented(true);
    REQUIRE(app->dependencyStatus() == DependencyStatus::Blocked);
    REQUIRE(app->unimplementedDependencies().size() == 1);
    REQUIRE(app->unimplementedDependencies().first() == io);

    // Implement the last dependency → app becomes ready.
    io->setImplemented(true);
    REQUIRE(app->dependencyStatus() == DependencyStatus::Ready);
    REQUIRE(app->unimplementedDependencies().isEmpty());
}

TEST_CASE("Scene reports which nodes are ready vs blocked", "[dependencies]")
{
    GraphScene scene;
    auto* app  = new GraphNode(NodeType::Module, "app");
    auto* core = new GraphNode(NodeType::Module, "core");
    scene.addItem(app);
    scene.addItem(core);
    scene.addItem(new GraphEdge(app, core));   // app depends on core

    // core is a leaf (ready); app is blocked by the unimplemented core.
    REQUIRE(scene.readyToImplement().contains(core));
    REQUIRE(scene.blocked().contains(app));
    REQUIRE_FALSE(scene.readyToImplement().contains(app));

    // Once core is implemented it drops out of the ready set and app enters it.
    core->setImplemented(true);
    REQUIRE_FALSE(scene.readyToImplement().contains(core));
    REQUIRE(scene.readyToImplement().contains(app));
    REQUIRE(scene.blocked().isEmpty());
}

TEST_CASE("Self-loops and duplicate edges do not create phantom dependencies",
          "[dependencies]")
{
    GraphScene scene;
    auto* a = new GraphNode(NodeType::Class, "A");
    auto* b = new GraphNode(NodeType::Class, "B");
    scene.addItem(a);
    scene.addItem(b);
    scene.addItem(new GraphEdge(a, a));   // self-loop
    scene.addItem(new GraphEdge(a, b));   // A → B
    scene.addItem(new GraphEdge(a, b));   // duplicate A → B

    REQUIRE(a->dependencies().size() == 1);
    REQUIRE(a->dependencies().first() == b);
}
