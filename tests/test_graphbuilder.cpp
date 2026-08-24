#include <catch2/catch_test_macros.hpp>

#include "graphbuilder.h"
#include "ir.h"
#include "graphscene.h"
#include "graphnode.h"
#include "graphedge.h"
#include "scene_helpers.h"

#include <QList>
#include <QString>

// GraphScene is a QGraphicsScene, so these tests need a live QApplication.
// tests/qt_app_fixture.cpp registers a Catch2 listener that creates one for the
// whole run — exactly as test_graphscene.cpp relies on. Nothing to do here.
//
// Every ir::Repo below is built BY HAND. That is the point of B2: the builder is
// tested independently of any analyzer, so these tests pin the IR -> scene
// contract and nothing else.

// ── Helpers ───────────────────────────────────────────────────────────────────

// Insertion order, oldest first. GraphScene::nodes() (and scene.items()) report
// descending stacking order, which for equal-Z items is reverse insertion order;
// asking for Qt::AscendingOrder gives back the order the builder created them in.
static QList<GraphNode*> nodesInCreationOrder(GraphScene& scene)
{
    QList<GraphNode*> result;
    for (QGraphicsItem* item : scene.items(Qt::AscendingOrder))
        if (auto* n = qgraphicsitem_cast<GraphNode*>(item))
            result.append(n);
    return result;
}

static int nodeCountNamed(GraphScene& scene, const QString& label)
{
    int count = 0;
    for (GraphNode* n : scene.nodes())
        if (n->label() == label) ++count;
    return count;
}

static ir::Function makeFunction(const QString& label, const QString& description)
{
    ir::Function f;
    f.label       = label;
    f.description = description;
    return f;
}

// The canonical fixture. It exercises all seven edge categories at once, and it
// deliberately also carries the labels that must be DROPPED (unresolved) or
// SUPPRESSED (self-referential), so the happy-path counts below double as a
// check that nothing spurious was emitted.
//
// Expected result: 8 nodes (3 modules, 3 types, 2 functions) and 9 edges.
//
//   app  -> core         "contains"     module -> module
//   app  -> io           "depends on"   module -> module
//   app  -> main         "contains"     module -> function
//   core -> Widget       "contains"     module -> class
//   core -> Button       "contains"     module -> class
//   io   -> Reader       "contains"     module -> class
//   Widget -> Widget::draw "provides"   class  -> function
//   Button -> Widget     "inherits"     class  -> class
//   Reader -> Widget     "uses"         class  -> class
static ir::Repo makeSampleRepo()
{
    ir::Repo repo;

    ir::Module app;
    app.label              = QStringLiteral("app");
    app.description        = QStringLiteral("The application shell.");
    app.childModuleLabels  = { QStringLiteral("core"),
                               QStringLiteral("ghost_module") };  // ghost: dropped
    app.dependsOnLabels    = { QStringLiteral("io"),
                               QStringLiteral("missing_lib"),     // dropped
                               QStringLiteral("app") };           // self: suppressed
    app.freeFunctions      = { makeFunction(QStringLiteral("main"),
                                            QStringLiteral("Program entry point.")) };
    repo.addModule(app);

    ir::Module core;
    core.label       = QStringLiteral("core");
    core.description = QStringLiteral("Core library.");
    repo.addModule(core);

    ir::Module io;
    io.label       = QStringLiteral("io");
    io.description = QStringLiteral("I/O library.");
    repo.addModule(io);

    ir::Type widget;
    widget.label       = QStringLiteral("Widget");
    widget.description = QStringLiteral("A drawable widget.");
    widget.moduleLabel = QStringLiteral("core");
    widget.methods     = { makeFunction(QStringLiteral("Widget::draw"),
                                        QStringLiteral("Paints the widget onto a device.")) };
    repo.addType(widget);

    ir::Type button;
    button.label       = QStringLiteral("Button");
    button.description = QStringLiteral("A clickable widget.");
    button.moduleLabel = QStringLiteral("core");
    button.baseLabels  = { QStringLiteral("Widget"),
                           QStringLiteral("NoSuchBase") };         // dropped
    repo.addType(button);

    ir::Type reader;
    reader.label       = QStringLiteral("Reader");
    reader.description = QStringLiteral("Loads widgets from disk.");
    reader.moduleLabel = QStringLiteral("io");
    reader.usesLabels  = { QStringLiteral("Widget"),
                           QStringLiteral("Reader"),               // self: suppressed
                           QStringLiteral("Nowhere") };            // dropped
    repo.addType(reader);

    return repo;
}

// ── Nodes ─────────────────────────────────────────────────────────────────────

TEST_CASE("Builder creates one node per module, type, and function in the IR",
          "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    REQUIRE(scene.nodes().size() == 8);

    CHECK(nodesOfKind(scene, NodeType::Module).size()   == 3);
    CHECK(nodesOfKind(scene, NodeType::Class).size()    == 3);
    CHECK(nodesOfKind(scene, NodeType::Function).size() == 2);

    CHECK(nodeNamed(scene, "app")  != nullptr);
    CHECK(nodeNamed(scene, "core") != nullptr);
    CHECK(nodeNamed(scene, "io")   != nullptr);
    CHECK(nodeNamed(scene, "Widget") != nullptr);
    CHECK(nodeNamed(scene, "Button") != nullptr);
    CHECK(nodeNamed(scene, "Reader") != nullptr);
    CHECK(nodeNamed(scene, "main")         != nullptr);
    CHECK(nodeNamed(scene, "Widget::draw") != nullptr);
}

TEST_CASE("Builder maps each IR kind onto the matching NodeType", "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    REQUIRE(nodeNamed(scene, "core")         != nullptr);
    REQUIRE(nodeNamed(scene, "Widget")       != nullptr);
    REQUIRE(nodeNamed(scene, "Widget::draw") != nullptr);
    REQUIRE(nodeNamed(scene, "main")         != nullptr);

    CHECK(nodeNamed(scene, "core")->kind()         == NodeType::Module);
    CHECK(nodeNamed(scene, "Widget")->kind()       == NodeType::Class);
    CHECK(nodeNamed(scene, "Widget::draw")->kind() == NodeType::Function);
    // A module's free function is a Function node, not a Module node.
    CHECK(nodeNamed(scene, "main")->kind()         == NodeType::Function);
}

TEST_CASE("Builder creates nodes in module, type, function order", "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    const QList<GraphNode*> ordered = nodesInCreationOrder(scene);
    REQUIRE(ordered.size() == 8);

    // Order is JSON array order and must stay deterministic: every Module comes
    // before every Class, and every Class before every Function.
    int lastBand = -1;
    for (GraphNode* n : ordered) {
        const int band = (n->kind() == NodeType::Module) ? 0
                       : (n->kind() == NodeType::Class)  ? 1
                                                         : 2;
        CHECK(band >= lastBand);
        lastBand = band;
    }

    // Within a band, IR append order is preserved.
    CHECK(ordered.at(0)->label() == "app");
    CHECK(ordered.at(1)->label() == "core");
    CHECK(ordered.at(2)->label() == "io");
    CHECK(ordered.at(3)->label() == "Widget");
    CHECK(ordered.at(4)->label() == "Button");
    CHECK(ordered.at(5)->label() == "Reader");
}

TEST_CASE("Builder marks every node it creates as implemented", "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    REQUIRE(scene.nodes().size() == 8);
    // Every node stands for code that already exists in the scanned repository.
    for (GraphNode* n : scene.nodes())
        CHECK(n->isImplemented());
}

TEST_CASE("Builder takes node descriptions from the IR rather than formatting them",
          "[graphbuilder]")
{
    ir::Repo repo;

    ir::Module tools;
    tools.label         = QStringLiteral("tools");
    tools.description   = QStringLiteral("Assorted command-line tools.");
    tools.freeFunctions = { makeFunction(
        QStringLiteral("normalisePath"),
        QStringLiteral("Collapses ../ segments; distinctive text 42.")) };
    repo.addModule(tools);

    ir::Type cache;
    cache.label       = QStringLiteral("Cache");
    cache.description = QStringLiteral("An LRU cache.");
    cache.moduleLabel = QStringLiteral("tools");
    cache.methods     = { makeFunction(QStringLiteral("Cache::evict"),
                                       QStringLiteral("Drops the least recent entry.")) };
    repo.addType(cache);

    GraphScene scene;
    GraphBuilder builder;
    builder.build(repo, &scene);

    REQUIRE(nodeNamed(scene, "normalisePath") != nullptr);
    REQUIRE(nodeNamed(scene, "Cache::evict")  != nullptr);

    // Verbatim: the builder must not decorate, prefix, or re-wrap the IR text.
    CHECK(nodeNamed(scene, "normalisePath")->comment()
          == "Collapses ../ segments; distinctive text 42.");
    CHECK(nodeNamed(scene, "Cache::evict")->comment()
          == "Drops the least recent entry.");
    CHECK(nodeNamed(scene, "Cache")->comment()  == "An LRU cache.");
    CHECK(nodeNamed(scene, "tools")->comment()  == "Assorted command-line tools.");
}

// ── Edges: the seven categories ───────────────────────────────────────────────

TEST_CASE("Builder emits contains edges from modules to nested modules, classes, "
          "and free functions", "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    // module -> module, from Module::childModuleLabels
    REQUIRE(hasEdge(scene, "app", "core"));
    CHECK(edgeLabel(scene, "app", "core") == "contains");

    // module -> class, from Type::moduleLabel
    REQUIRE(hasEdge(scene, "core", "Widget"));
    CHECK(edgeLabel(scene, "core", "Widget") == "contains");
    REQUIRE(hasEdge(scene, "core", "Button"));
    CHECK(edgeLabel(scene, "core", "Button") == "contains");
    REQUIRE(hasEdge(scene, "io", "Reader"));
    CHECK(edgeLabel(scene, "io", "Reader") == "contains");

    // module -> function, from Module::freeFunctions
    REQUIRE(hasEdge(scene, "app", "main"));
    CHECK(edgeLabel(scene, "app", "main") == "contains");
}

TEST_CASE("Builder emits provides edges from classes to their methods", "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    // class -> function, from Type::methods
    REQUIRE(hasEdge(scene, "Widget", "Widget::draw"));
    CHECK(edgeLabel(scene, "Widget", "Widget::draw") == "provides");

    // The method belongs to its class, not to the class's module.
    CHECK_FALSE(hasEdge(scene, "core", "Widget::draw"));
}

TEST_CASE("Builder emits inherits edges from derived classes to their bases",
          "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    // class -> class, from Type::baseLabels
    REQUIRE(hasEdge(scene, "Button", "Widget"));
    CHECK(edgeLabel(scene, "Button", "Widget") == "inherits");

    // Direction matters: the derived class depends on the base, not vice versa.
    CHECK_FALSE(hasEdge(scene, "Widget", "Button"));
}

TEST_CASE("Builder emits uses edges between classes", "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    // class -> class, from Type::usesLabels
    REQUIRE(hasEdge(scene, "Reader", "Widget"));
    CHECK(edgeLabel(scene, "Reader", "Widget") == "uses");
}

TEST_CASE("Builder emits depends on edges between modules", "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    // module -> module, from Module::dependsOnLabels
    REQUIRE(hasEdge(scene, "app", "io"));
    CHECK(edgeLabel(scene, "app", "io") == "depends on");

    // "contains" and "depends on" are distinct categories over the same pair of
    // kinds, so the nesting edge must not be relabelled.
    CHECK(edgeLabel(scene, "app", "core") == "contains");
}

TEST_CASE("Builder emits exactly the seven edge categories and nothing else",
          "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    // Nine edges: the seven categories, with module -> class occurring three
    // times. Everything else in the fixture is an unresolved or self label.
    CHECK(edgeCount(scene) == 9);
}

// ── Unresolved labels, self-edges, duplicates, emptiness ──────────────────────

TEST_CASE("Builder silently drops edge labels that name no node", "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    // No node was invented for any of the dangling labels...
    CHECK(nodeNamed(scene, "ghost_module") == nullptr);
    CHECK(nodeNamed(scene, "missing_lib")  == nullptr);
    CHECK(nodeNamed(scene, "NoSuchBase")   == nullptr);
    CHECK(nodeNamed(scene, "Nowhere")      == nullptr);

    // ...and no edge points at one.
    CHECK_FALSE(hasEdge(scene, "app", "ghost_module"));
    CHECK_FALSE(hasEdge(scene, "app", "missing_lib"));
    CHECK_FALSE(hasEdge(scene, "Button", "NoSuchBase"));
    CHECK_FALSE(hasEdge(scene, "Reader", "Nowhere"));

    // The resolvable siblings in those same lists still produced their edges,
    // so a bad label drops one entry and not the whole list.
    CHECK(hasEdge(scene, "app", "core"));
    CHECK(hasEdge(scene, "app", "io"));
    CHECK(hasEdge(scene, "Button", "Widget"));
    CHECK(hasEdge(scene, "Reader", "Widget"));
}

TEST_CASE("Builder drops a type whose owning module label does not resolve",
          "[graphbuilder]")
{
    ir::Repo repo;

    ir::Module core;
    core.label = QStringLiteral("core");
    repo.addModule(core);

    ir::Type orphan;
    orphan.label       = QStringLiteral("Orphan");
    orphan.moduleLabel = QStringLiteral("no_such_module");
    repo.addType(orphan);

    ir::Type homeless;
    homeless.label       = QStringLiteral("Homeless");
    homeless.moduleLabel = QString();   // never set by the analyzer
    repo.addType(homeless);

    GraphScene scene;
    GraphBuilder builder;
    builder.build(repo, &scene);

    // The type node itself still exists; only the containment edge is dropped.
    REQUIRE(nodeNamed(scene, "Orphan")   != nullptr);
    REQUIRE(nodeNamed(scene, "Homeless") != nullptr);
    CHECK(nodeNamed(scene, "no_such_module") == nullptr);
    CHECK(edgeCount(scene) == 0);
}

TEST_CASE("Builder suppresses self-edges", "[graphbuilder]")
{
    ir::Repo repo;

    ir::Module solo;
    solo.label           = QStringLiteral("solo");
    solo.dependsOnLabels = { QStringLiteral("solo") };
    solo.childModuleLabels = { QStringLiteral("solo") };
    repo.addModule(solo);

    ir::Type recursive;
    recursive.label       = QStringLiteral("Recursive");
    recursive.moduleLabel = QStringLiteral("solo");
    recursive.usesLabels  = { QStringLiteral("Recursive") };
    recursive.baseLabels  = { QStringLiteral("Recursive") };
    repo.addType(recursive);

    GraphScene scene;
    GraphBuilder builder;
    builder.build(repo, &scene);

    REQUIRE(nodeNamed(scene, "solo")      != nullptr);
    REQUIRE(nodeNamed(scene, "Recursive") != nullptr);

    CHECK_FALSE(hasEdge(scene, "solo", "solo"));
    CHECK_FALSE(hasEdge(scene, "Recursive", "Recursive"));

    // Only the module -> class containment edge survives.
    CHECK(edgeCount(scene) == 1);
    CHECK(hasEdge(scene, "solo", "Recursive"));
}

TEST_CASE("Builder keeps only the first node for a duplicated label", "[graphbuilder]")
{
    ir::Repo repo;

    ir::Module core;
    core.label = QStringLiteral("core");
    repo.addModule(core);

    ir::Module other;
    other.label = QStringLiteral("other");
    repo.addModule(other);

    // Two headers in different directories declaring the same class name: the
    // original scanner's classByName map kept whichever it saw first.
    ir::Type first;
    first.label       = QStringLiteral("Widget");
    first.description = QStringLiteral("first wins");
    first.moduleLabel = QStringLiteral("core");
    repo.addType(first);

    ir::Type second;
    second.label       = QStringLiteral("Widget");
    second.description = QStringLiteral("second loses");
    second.moduleLabel = QStringLiteral("other");
    repo.addType(second);

    GraphScene scene;
    GraphBuilder builder;
    builder.build(repo, &scene);

    REQUIRE(nodeCountNamed(scene, QStringLiteral("Widget")) == 1);
    CHECK(nodeNamed(scene, "Widget")->comment() == "first wins");
    CHECK(nodesOfKind(scene, NodeType::Class).size() == 1);
}

TEST_CASE("Builder clears the scene before rebuilding it", "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;
    const ir::Repo repo = makeSampleRepo();

    builder.build(repo, &scene);
    const int nodesAfterFirst = scene.nodes().size();
    const int edgesAfterFirst = edgeCount(scene);
    REQUIRE(nodesAfterFirst == 8);
    REQUIRE(edgesAfterFirst == 9);

    // Building again over the same scene must replace, not accumulate.
    builder.build(repo, &scene);
    CHECK(scene.nodes().size() == nodesAfterFirst);
    CHECK(edgeCount(scene)     == edgesAfterFirst);
    CHECK(nodeCountNamed(scene, QStringLiteral("Widget")) == 1);
    CHECK(nodeCountNamed(scene, QStringLiteral("app"))    == 1);
    CHECK(hasEdge(scene, "Button", "Widget"));
}

TEST_CASE("Builder clears hand-made items already in the scene", "[graphbuilder]")
{
    GraphScene scene;

    // Something the user drew before the analyzer ran.
    auto* stale = new GraphNode(NodeType::Class, QStringLiteral("HandDrawn"));
    scene.addItem(stale);
    REQUIRE(scene.nodes().size() == 1);

    GraphBuilder builder;
    builder.build(makeSampleRepo(), &scene);

    CHECK(nodeNamed(scene, "HandDrawn") == nullptr);
    CHECK(scene.nodes().size() == 8);
}

TEST_CASE("Builder turns an empty repo into an empty scene", "[graphbuilder]")
{
    const ir::Repo repo;
    REQUIRE(repo.isEmpty());

    GraphScene scene;
    GraphBuilder builder;
    builder.build(repo, &scene);

    CHECK(scene.nodes().isEmpty());
    CHECK(edgeCount(scene) == 0);
}

TEST_CASE("Builder empties a populated scene when given an empty repo", "[graphbuilder]")
{
    GraphScene scene;
    GraphBuilder builder;

    builder.build(makeSampleRepo(), &scene);
    REQUIRE(scene.nodes().size() == 8);

    builder.build(ir::Repo(), &scene);
    CHECK(scene.nodes().isEmpty());
    CHECK(edgeCount(scene) == 0);
}
