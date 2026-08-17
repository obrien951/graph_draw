#include <catch2/catch_all.hpp>
#include "repoanalyzer.h"
#include "graphscene.h"
#include "graphnode.h"
#include "graphedge.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

// ── Helpers ───────────────────────────────────────────────────────────────────

static void writeFile(const QString& path, const QString& contents)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(contents.toUtf8());
    f.close();
}

static QList<GraphNode*> nodesOfKind(GraphScene& scene, NodeType kind)
{
    QList<GraphNode*> result;
    for (GraphNode* n : scene.nodes())
        if (n->kind() == kind) result.append(n);
    return result;
}

static GraphNode* nodeNamed(GraphScene& scene, const QString& label)
{
    for (GraphNode* n : scene.nodes())
        if (n->label() == label) return n;
    return nullptr;
}

static bool hasEdge(GraphScene& scene, const QString& from, const QString& to)
{
    for (QGraphicsItem* item : scene.items())
        if (auto* e = qgraphicsitem_cast<GraphEdge*>(item))
            if (e->source() && e->target() &&
                e->source()->label() == from && e->target()->label() == to)
                return true;
    return false;
}

// Build a small two-library repo on disk for the analyzer to scan.
static void buildSampleRepo(const QDir& root)
{
    root.mkpath(QStringLiteral("core"));
    root.mkpath(QStringLiteral("io"));

    writeFile(root.filePath("core/CMakeLists.txt"),
              "add_library(core STATIC widget.h widget.cpp)\n");
    writeFile(root.filePath("io/CMakeLists.txt"),
              "add_library(io STATIC reader.h)\n"
              "target_link_libraries(io PUBLIC core)\n");

    writeFile(root.filePath("core/widget.h"),
              "#pragma once\n"
              "// A drawable widget.\n"
              "class Widget {\n"
              "public:\n"
              "    void draw();\n"
              "    int  area() const;\n"
              "    Widget();            // constructor must be ignored\n"
              "    ~Widget();           // destructor must be ignored\n"
              "};\n"
              "class Button : public Widget {\n"
              "public:\n"
              "    void click(const QString& label = QString());\n"
              "};\n");
    writeFile(root.filePath("core/widget.cpp"),
              "#include \"widget.h\"\n"
              "void Widget::draw() {}\n");
    writeFile(root.filePath("io/reader.h"),
              "#pragma once\n"
              "#include \"widget.h\"\n"
              "class Reader {\n"
              "public:\n"
              "    Widget load();\n"
              "};\n");
}

// ── Tests ──────────────────────────────────────────────────────────────────────

TEST_CASE("Analyzer maps CMake targets to Module nodes", "[repoanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    GraphScene scene;
    RepoAnalyzer analyzer;
    REQUIRE(analyzer.analyze(root.absolutePath(), &scene));

    REQUIRE(nodesOfKind(scene, NodeType::Module).size() == 2);
    REQUIRE(nodeNamed(scene, "core") != nullptr);
    REQUIRE(nodeNamed(scene, "io")   != nullptr);
    REQUIRE(nodeNamed(scene, "core")->kind() == NodeType::Module);
    // Every discovered node represents existing code, so it is implemented.
    REQUIRE(nodeNamed(scene, "core")->isImplemented());
}

TEST_CASE("Analyzer finds classes and attaches them to their module",
          "[repoanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    GraphScene scene;
    RepoAnalyzer analyzer;
    REQUIRE(analyzer.analyze(root.absolutePath(), &scene));

    REQUIRE(nodeNamed(scene, "Widget") != nullptr);
    REQUIRE(nodeNamed(scene, "Button") != nullptr);
    REQUIRE(nodeNamed(scene, "Reader") != nullptr);

    // module → class ("contains")
    REQUIRE(hasEdge(scene, "core", "Widget"));
    REQUIRE(hasEdge(scene, "io",   "Reader"));

    // The leading comment above a class becomes its description.
    REQUIRE(nodeNamed(scene, "Widget")->comment() == "A drawable widget.");
}

TEST_CASE("Analyzer extracts member functions but skips ctors/dtors and defaults",
          "[repoanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    GraphScene scene;
    RepoAnalyzer analyzer;
    REQUIRE(analyzer.analyze(root.absolutePath(), &scene));

    REQUIRE(nodeNamed(scene, "Widget::draw") != nullptr);
    REQUIRE(nodeNamed(scene, "Widget::area") != nullptr);
    REQUIRE(hasEdge(scene, "Widget", "Widget::draw"));    // class → function

    // Constructors, destructors, and default-argument artifacts are not functions.
    REQUIRE(nodeNamed(scene, "Widget::Widget")  == nullptr);
    REQUIRE(nodeNamed(scene, "Widget::~Widget") == nullptr);
    REQUIRE(nodeNamed(scene, "Button::QString") == nullptr);
    REQUIRE(nodeNamed(scene, "Button::click")   != nullptr);
}

TEST_CASE("Analyzer records inheritance, include-based use, and module deps",
          "[repoanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    GraphScene scene;
    RepoAnalyzer analyzer;
    REQUIRE(analyzer.analyze(root.absolutePath(), &scene));

    REQUIRE(hasEdge(scene, "Button", "Widget"));   // inherits
    REQUIRE(hasEdge(scene, "Reader", "Widget"));   // uses (reader.h includes widget.h)
    REQUIRE(hasEdge(scene, "io",     "core"));     // depends on (target_link_libraries)
}
