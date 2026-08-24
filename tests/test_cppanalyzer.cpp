#include <catch2/catch_test_macros.hpp>

#include "cppanalyzer.h"
#include "repofileindex.h"
#include "ir.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

//
// IR-level regression contract for the C++ scanner (B3).
//
// tests/test_repoanalyzer.cpp already pins this behaviour end to end, through a
// GraphScene and therefore through a QApplication. This file pins the SAME
// behaviour one layer down, on the ir::Repo the analyzer produces, and it does
// so with Qt Core only: no graphscene.h, no graphnode.h, no graphedge.h, no
// scene_helpers.h. That absence is the point. graph_lang and graph_lang_cpp link
// Qt Core alone, so a language analyzer structurally cannot build scene items,
// and its tests need no QApplication. If this file ever needs a widgets include
// to compile, the layering has been broken.
//
// Everything asserted here is what graph_analyze/repoanalyzer.cpp did before the
// move, including the parts that look like bugs. Where the original's behaviour
// is surprising the test says so rather than "correcting" it.
//

// ── Fixture ───────────────────────────────────────────────────────────────────

namespace {

void writeFile(const QString& path, const QString& contents)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(contents.toUtf8());
    f.close();
}

// A small polyglot-free C++ tree exercising every branch of the scanner:
// two CMake libraries, one CMake executable, three directory-fallback modules,
// a duplicate class name, generated sources, and a class at the repo root.
//
//   app/CMakeLists.txt          add_executable + target_link_libraries
//   app/main.cpp                owned by "app", defines no class
//   core/CMakeLists.txt         add_library(core ...)
//   core/widget.h               class Widget      (leading // comment)
//   core/widget.cpp             out-of-line definition, no class
//   core/button.h               class Button : public Widget
//   io/CMakeLists.txt           add_library(io ...) + link to core
//   io/reader.h                 class Reader      (leading /* */ comment)
//   dup/first.h                 class Dup         (first definition wins)
//   dup/second.h                class Dup         (loses)
//   gen/*                       generated sources, all ignored
//   orphan.h                    struct Point at the repo root
//   tools/commented.h           class Commented, include hidden in a comment
//   tools/fancy.h               class Fancy, qualified/templated/virtual bases
//   tools/gadget.h              class Gadget + an `enum class`
//   tools/selfref.h             class SelfRef, header that includes itself
//   tools/sub/deep.h            class Deep, nested below the fallback directory
//
// Include directives are written in ascending basename order throughout, so the
// expected "uses" lists are unambiguous.
void buildSampleRepo(const QDir& root)
{
    root.mkpath(QStringLiteral("app"));
    root.mkpath(QStringLiteral("core"));
    root.mkpath(QStringLiteral("io"));
    root.mkpath(QStringLiteral("dup"));
    root.mkpath(QStringLiteral("gen"));
    root.mkpath(QStringLiteral("tools/sub"));

    // ── CMake targets ────────────────────────────────────────────────────────
    writeFile(root.filePath(QStringLiteral("app/CMakeLists.txt")),
              QStringLiteral(
                  "add_executable(app main.cpp)\n"
                  "target_link_libraries(app PRIVATE io INTERFACE core)\n"));
    writeFile(root.filePath(QStringLiteral("core/CMakeLists.txt")),
              QStringLiteral(
                  "add_library(core STATIC widget.h widget.cpp button.h)\n"));
    writeFile(root.filePath(QStringLiteral("io/CMakeLists.txt")),
              QStringLiteral(
                  "add_library(io STATIC reader.h)\n"
                  "target_link_libraries(io PUBLIC core Qt6::Core)\n"));

    // ── Sources owned by CMake targets ───────────────────────────────────────
    writeFile(root.filePath(QStringLiteral("app/main.cpp")),
              QStringLiteral("#include \"reader.h\"\n"
                             "int main() { return 0; }\n"));

    writeFile(root.filePath(QStringLiteral("core/widget.h")),
              QStringLiteral(
                  "#pragma once\n"
                  "// A drawable widget.\n"
                  "class Widget {\n"
                  "public:\n"
                  "    void draw();\n"
                  "    int  area() const;\n"
                  "    Widget();                       // ctor: ignored\n"
                  "    ~Widget();                      // dtor: ignored\n"
                  "    virtual void render() = 0;\n"
                  "    void setCallback(int (*fn)(int));\n"
                  "    bool operator==(const Widget& other) const;\n"
                  "    void click(const QString& label = QString());\n"
                  "    static const int kMax = sizeof(int);\n"
                  "};\n"));

    writeFile(root.filePath(QStringLiteral("core/widget.cpp")),
              QStringLiteral("#include \"widget.h\"\n"
                             "void Widget::draw() {}\n"));

    writeFile(root.filePath(QStringLiteral("core/button.h")),
              QStringLiteral(
                  "#pragma once\n"
                  "#include \"widget.h\"\n"
                  "// A clickable widget.\n"
                  "class Button : public Widget {\n"
                  "public:\n"
                  "    void click(const QString& label = QString());\n"
                  "};\n"));

    writeFile(root.filePath(QStringLiteral("io/reader.h")),
              QStringLiteral(
                  "#pragma once\n"
                  "#include \"button.h\"\n"
                  "#include \"gadget.h\"\n"
                  "#include \"widget.h\"\n"
                  "/* Reads widgets. */\n"
                  "class Reader {\n"
                  "public:\n"
                  "    Widget load();\n"
                  "};\n"));

    // ── Duplicate class name: the first file scanned wins ────────────────────
    writeFile(root.filePath(QStringLiteral("dup/first.h")),
              QStringLiteral("#pragma once\n"
                             "// The winning definition.\n"
                             "class Dup {\n"
                             "public:\n"
                             "    void first();\n"
                             "};\n"));
    writeFile(root.filePath(QStringLiteral("dup/second.h")),
              QStringLiteral("#pragma once\n"
                             "// The losing definition.\n"
                             "class Dup {\n"
                             "public:\n"
                             "    void second();\n"
                             "};\n"));

    // ── Generated sources: every one of these must be invisible ──────────────
    writeFile(root.filePath(QStringLiteral("gen/moc_widget.cpp")),
              QStringLiteral("class MocGarbage {};\n"));
    writeFile(root.filePath(QStringLiteral("gen/qrc_res.cpp")),
              QStringLiteral("class QrcGarbage {};\n"));
    writeFile(root.filePath(QStringLiteral("gen/ui_form.h")),
              QStringLiteral("class UiGarbage {};\n"));
    writeFile(root.filePath(QStringLiteral("gen/thing_autogen.cpp")),
              QStringLiteral("class AutogenGarbage {};\n"));
    writeFile(root.filePath(QStringLiteral("gen/mocs_compilation.cpp")),
              QStringLiteral("class MocsGarbage {};\n"));

    // ── A class at the repo root, owned by no CMake target ───────────────────
    writeFile(root.filePath(QStringLiteral("orphan.h")),
              QStringLiteral("#pragma once\n"
                             "struct Point {\n"
                             "    int x() const;\n"
                             "    int y() const;\n"
                             "};\n"
                             "enum class Axis { X, Y };\n"));

    // ── Directory-fallback sources under tools/ ──────────────────────────────
    writeFile(root.filePath(QStringLiteral("tools/commented.h")),
              QStringLiteral(
                  "#pragma once\n"
                  "// Historical note: #include \"widget.h\" was removed.\n"
                  "\n"
                  "// Includes hidden in comments are still seen.\n"
                  "class Commented {\n"
                  "public:\n"
                  "    void note();\n"
                  "};\n"));

    writeFile(root.filePath(QStringLiteral("tools/fancy.h")),
              QStringLiteral(
                  "#pragma once\n"
                  "// Bases with qualifiers, templates and namespaces.\n"
                  "class Fancy : public virtual ns::Base<int>, private Widget {\n"
                  "};\n"));

    writeFile(root.filePath(QStringLiteral("tools/gadget.h")),
              QStringLiteral(
                  "#pragma once\n"
                  "// A gadget.\n"
                  "class Gadget {\n"
                  "public:\n"
                  "    void spin() { for (int i = 0; i < 3; ++i) { tick(); } }\n"
                  "};\n"
                  "enum class Mode { Fast, Slow };\n"));

    writeFile(root.filePath(QStringLiteral("tools/selfref.h")),
              QStringLiteral("#pragma once\n"
                             "#include \"selfref.h\"\n"
                             "// A self-referential header.\n"
                             "class SelfRef {\n"
                             "public:\n"
                             "    void ping();\n"
                             "};\n"));

    writeFile(root.filePath(QStringLiteral("tools/sub/deep.h")),
              QStringLiteral("#pragma once\n"
                             "class Deep {};\n"));
}

// Fetch a type/module by label, failing the test rather than dereferencing null.
const ir::Type& typeNamed(const ir::Repo& repo, const QString& label)
{
    const ir::Type* t = repo.type(label);
    REQUIRE(t != nullptr);
    return *t;
}

const ir::Module& moduleNamed(const ir::Repo& repo, const QString& label)
{
    const ir::Module* m = repo.module(label);
    REQUIRE(m != nullptr);
    return *m;
}

QStringList methodLabels(const ir::Type& t)
{
    QStringList out;
    for (const ir::Function& f : t.methods)
        out.append(f.label);
    return out;
}

QStringList sortedCopy(QStringList list)
{
    list.sort();
    return list;
}

// Analyze `root` into a fresh Repo, requiring success.
ir::Repo analyzeRepo(const QString& root)
{
    const RepoFileIndex index = buildRepoFileIndex(root);
    ir::Repo repo;
    QString error;
    REQUIRE(CppAnalyzer().analyze(index, &repo, &error));
    REQUIRE(error.isEmpty());
    return repo;
}

} // namespace

// ── Identity and detection ────────────────────────────────────────────────────

TEST_CASE("The C++ analyzer identifies itself as cpp", "[cppanalyzer]")
{
    REQUIRE(CppAnalyzer().id() == QStringLiteral("cpp"));
}

TEST_CASE("The C++ analyzer detects a tree holding CMakeLists.txt or C++ sources",
          "[cppanalyzer]")
{
    const CppAnalyzer analyzer;

    QTemporaryDir sample;
    REQUIRE(sample.isValid());
    buildSampleRepo(QDir(sample.path()));
    CHECK(analyzer.detect(buildRepoFileIndex(sample.path())));

    // CMakeLists.txt alone is enough, even with no source beside it.
    QTemporaryDir cmakeOnly;
    REQUIRE(cmakeOnly.isValid());
    writeFile(QDir(cmakeOnly.path()).filePath(QStringLiteral("CMakeLists.txt")),
              QStringLiteral("project(empty)\n"));
    CHECK(analyzer.detect(buildRepoFileIndex(cmakeOnly.path())));

    // A single C++ source with no build system is also enough.
    QTemporaryDir sourceOnly;
    REQUIRE(sourceOnly.isValid());
    writeFile(QDir(sourceOnly.path()).filePath(QStringLiteral("main.cpp")),
              QStringLiteral("int main() { return 0; }\n"));
    CHECK(analyzer.detect(buildRepoFileIndex(sourceOnly.path())));
}

TEST_CASE("The C++ analyzer does not detect a tree with neither CMake nor C++",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    writeFile(root.filePath(QStringLiteral("notes.txt")),
              QStringLiteral("just some prose\n"));
    writeFile(root.filePath(QStringLiteral("lib.rs")),
              QStringLiteral("pub struct Thing;\n"));

    CHECK_FALSE(CppAnalyzer().detect(buildRepoFileIndex(tmp.path())));

    // An index over a root that does not exist is empty, and empty is not C++.
    CHECK_FALSE(CppAnalyzer().detect(
        buildRepoFileIndex(root.filePath(QStringLiteral("no_such_dir")))));
}

// ── Modules ───────────────────────────────────────────────────────────────────

TEST_CASE("Modules come from add_library and add_executable target names",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    REQUIRE(repo.hasModule(QStringLiteral("core")));
    REQUIRE(repo.hasModule(QStringLiteral("io")));
    REQUIRE(repo.hasModule(QStringLiteral("app")));

    // The description records the target kind, its directory relative to the
    // repo root, and how many sources the target listed.
    CHECK(moduleNamed(repo, QStringLiteral("core")).description ==
          QStringLiteral("CMake library target \"core\" (core), 3 source file(s)."));
    CHECK(moduleNamed(repo, QStringLiteral("app")).description ==
          QStringLiteral("CMake executable target \"app\" (app), 1 source file(s)."));
}

TEST_CASE("A source owned by no CMake target falls back to a directory module",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    // The fallback name is the FIRST path component plus " (dir)", so a file
    // nested at tools/sub/deep.h still lands in "tools (dir)".
    REQUIRE(repo.hasModule(QStringLiteral("tools (dir)")));
    CHECK(moduleNamed(repo, QStringLiteral("tools (dir)")).description ==
          QStringLiteral("Directory module \"tools\", 5 source file(s)."));
    CHECK(typeNamed(repo, QStringLiteral("Deep")).moduleLabel ==
          QStringLiteral("tools (dir)"));

    // A file directly at the repo root has no directory component at all and
    // gets the literal placeholder "(root)".
    REQUIRE(repo.hasModule(QStringLiteral("(root) (dir)")));
    CHECK(moduleNamed(repo, QStringLiteral("(root) (dir)")).description ==
          QStringLiteral("Directory module \"(root)\", 1 source file(s)."));
    CHECK(typeNamed(repo, QStringLiteral("Point")).moduleLabel ==
          QStringLiteral("(root) (dir)"));

    // A directory whose sources are all generated produces no module at all.
    CHECK_FALSE(repo.hasModule(QStringLiteral("gen (dir)")));
}

TEST_CASE("Module dependencies come from target_link_libraries with scope "
          "keywords skipped",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    // PUBLIC is dropped; an unresolvable library name is NOT — the IR keeps
    // every link name and the builder drops the ones that match no module.
    REQUIRE(repo.hasModule(QStringLiteral("io")));
    CHECK(moduleNamed(repo, QStringLiteral("io")).dependsOnLabels ==
          QStringList({QStringLiteral("core"), QStringLiteral("Qt6::Core")}));

    // PRIVATE and INTERFACE are dropped the same way, and argument order is
    // preserved.
    REQUIRE(repo.hasModule(QStringLiteral("app")));
    CHECK(moduleNamed(repo, QStringLiteral("app")).dependsOnLabels ==
          QStringList({QStringLiteral("io"), QStringLiteral("core")}));

    // A target that links nothing has no dependencies.
    REQUIRE(repo.hasModule(QStringLiteral("core")));
    CHECK(moduleNamed(repo, QStringLiteral("core")).dependsOnLabels.isEmpty());
}

TEST_CASE("Every module and type discovered in the sample tree is accounted for",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    CHECK(sortedCopy(repo.moduleLabels()) ==
          QStringList({QStringLiteral("(root) (dir)"), QStringLiteral("app"),
                       QStringLiteral("core"), QStringLiteral("dup (dir)"),
                       QStringLiteral("io"), QStringLiteral("tools (dir)")}));

    CHECK(sortedCopy(repo.typeLabels()) ==
          QStringList({QStringLiteral("Button"), QStringLiteral("Commented"),
                       QStringLiteral("Deep"), QStringLiteral("Dup"),
                       QStringLiteral("Fancy"), QStringLiteral("Gadget"),
                       QStringLiteral("Point"), QStringLiteral("Reader"),
                       QStringLiteral("SelfRef"), QStringLiteral("Widget")}));
}

// ── Types ─────────────────────────────────────────────────────────────────────

TEST_CASE("Types come from class and struct definitions but never from enum class",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    CHECK(repo.hasType(QStringLiteral("Widget")));   // class
    CHECK(repo.hasType(QStringLiteral("Point")));    // struct

    // "enum class Mode" and "enum class Axis" both match the class regex and are
    // rejected by the preceding "enum" keyword.
    CHECK_FALSE(repo.hasType(QStringLiteral("Mode")));
    CHECK_FALSE(repo.hasType(QStringLiteral("Axis")));
}

TEST_CASE("Types resolve the module that owns the file they were declared in",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    CHECK(typeNamed(repo, QStringLiteral("Widget")).moduleLabel == QStringLiteral("core"));
    CHECK(typeNamed(repo, QStringLiteral("Button")).moduleLabel == QStringLiteral("core"));
    CHECK(typeNamed(repo, QStringLiteral("Reader")).moduleLabel == QStringLiteral("io"));
    CHECK(typeNamed(repo, QStringLiteral("Dup")).moduleLabel == QStringLiteral("dup (dir)"));
    CHECK(typeNamed(repo, QStringLiteral("Gadget")).moduleLabel ==
          QStringLiteral("tools (dir)"));
}

TEST_CASE("The first definition of a class name wins when two files declare it",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    CHECK(repo.typeLabels().count(QStringLiteral("Dup")) == 1);

    // Files are scanned in index order, so dup/first.h beats dup/second.h.
    const ir::Type* dup = repo.type(QStringLiteral("Dup"));
    REQUIRE(dup != nullptr);
    CHECK(dup->description == QStringLiteral("The winning definition."));
    CHECK(methodLabels(*dup) == QStringList({QStringLiteral("Dup::first")}));
}

TEST_CASE("Generated sources contribute neither types nor modules", "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    CHECK_FALSE(repo.hasType(QStringLiteral("MocGarbage")));       // moc_*
    CHECK_FALSE(repo.hasType(QStringLiteral("QrcGarbage")));       // qrc_*
    CHECK_FALSE(repo.hasType(QStringLiteral("UiGarbage")));        // ui_*
    CHECK_FALSE(repo.hasType(QStringLiteral("AutogenGarbage")));   // *_autogen.cpp
    CHECK_FALSE(repo.hasType(QStringLiteral("MocsGarbage")));      // mocs_compilation.cpp
}

// ── Inheritance ───────────────────────────────────────────────────────────────

TEST_CASE("Base classes are recorded with access specifiers and qualifiers "
          "stripped",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    REQUIRE(repo.hasType(QStringLiteral("Button")));
    CHECK(typeNamed(repo, QStringLiteral("Button")).baseLabels ==
          QStringList({QStringLiteral("Widget")}));

    // "public virtual ns::Base<int>" loses the access specifier, "virtual", the
    // template argument list and the namespace qualification, in that order.
    // "Base" resolves to no type here and is still kept: dropping unresolved
    // endpoints is the builder's job, not the analyzer's.
    REQUIRE(repo.hasType(QStringLiteral("Fancy")));
    CHECK(typeNamed(repo, QStringLiteral("Fancy")).baseLabels ==
          QStringList({QStringLiteral("Base"), QStringLiteral("Widget")}));

    CHECK(typeNamed(repo, QStringLiteral("Widget")).baseLabels.isEmpty());
}

// ── Methods ───────────────────────────────────────────────────────────────────

TEST_CASE("Member functions become Class::method entries in declaration order",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    const ir::Type* widget = repo.type(QStringLiteral("Widget"));
    REQUIRE(widget != nullptr);

    // Present: plain, const, pure virtual, and a function-pointer parameter.
    // Absent: Widget() and ~Widget() (constructor/destructor), operator== (an
    // operator), QString (a default-argument artifact, because only the first
    // name before a '(' in a declaration is the declared function), and sizeof
    // (a non-function word), which also keeps kMax out.
    CHECK(methodLabels(*widget) ==
          QStringList({QStringLiteral("Widget::draw"),
                       QStringLiteral("Widget::area"),
                       QStringLiteral("Widget::render"),
                       QStringLiteral("Widget::setCallback"),
                       QStringLiteral("Widget::click")}));

    CHECK(methodLabels(typeNamed(repo, QStringLiteral("Button"))) ==
          QStringList({QStringLiteral("Button::click")}));
    CHECK(methodLabels(typeNamed(repo, QStringLiteral("Reader"))) ==
          QStringList({QStringLiteral("Reader::load")}));
    CHECK(methodLabels(typeNamed(repo, QStringLiteral("Point"))) ==
          QStringList({QStringLiteral("Point::x"), QStringLiteral("Point::y")}));

    // An empty class body yields no methods at all.
    CHECK(methodLabels(typeNamed(repo, QStringLiteral("Deep"))).isEmpty());
}

TEST_CASE("Inline bodies do not leak calls or control-flow keywords into methods",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    // Gadget::spin has an inline body containing `for (...)` and a `tick()`
    // call. Nested braces are collapsed before names are matched, so neither
    // becomes a member function.
    const ir::Type* gadget = repo.type(QStringLiteral("Gadget"));
    REQUIRE(gadget != nullptr);
    CHECK(methodLabels(*gadget) == QStringList({QStringLiteral("Gadget::spin")}));
}

TEST_CASE("Method descriptions name the owning class, keyword and source file",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    const ir::Type* widget = repo.type(QStringLiteral("Widget"));
    REQUIRE(widget != nullptr);
    REQUIRE_FALSE(widget->methods.isEmpty());
    CHECK(widget->methods.first().description ==
          QStringLiteral("Member function Widget::draw of class Widget "
                         "(core/widget.h)."));

    // The keyword tracks the declaration, so a struct says "struct".
    const ir::Type* point = repo.type(QStringLiteral("Point"));
    REQUIRE(point != nullptr);
    REQUIRE_FALSE(point->methods.isEmpty());
    CHECK(point->methods.first().description ==
          QStringLiteral("Member function Point::x of struct Point (orphan.h)."));
}

// ── Descriptions ──────────────────────────────────────────────────────────────

TEST_CASE("A leading comment above a class becomes its description",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    CHECK(typeNamed(repo, QStringLiteral("Widget")).description ==
          QStringLiteral("A drawable widget."));
    CHECK(typeNamed(repo, QStringLiteral("Button")).description ==
          QStringLiteral("A clickable widget."));

    // A block comment works too, with its markers stripped.
    CHECK(typeNamed(repo, QStringLiteral("Reader")).description ==
          QStringLiteral("Reads widgets."));
}

TEST_CASE("A class with no leading comment falls back to a declared-in description",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    // "<keyword> <name> declared in <path relative to the repo root>."
    CHECK(typeNamed(repo, QStringLiteral("Point")).description ==
          QStringLiteral("struct Point declared in orphan.h."));
    CHECK(typeNamed(repo, QStringLiteral("Deep")).description ==
          QStringLiteral("class Deep declared in tools/sub/deep.h."));
}

// ── Uses ──────────────────────────────────────────────────────────────────────

TEST_CASE("Uses come from including a header that defines another class",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    // io/reader.h includes button.h, gadget.h and widget.h.
    REQUIRE(repo.hasType(QStringLiteral("Reader")));
    CHECK(typeNamed(repo, QStringLiteral("Reader")).usesLabels ==
          QStringList({QStringLiteral("Button"), QStringLiteral("Gadget"),
                       QStringLiteral("Widget")}));

    // A file that includes nothing local uses nothing.
    CHECK(typeNamed(repo, QStringLiteral("Widget")).usesLabels.isEmpty());
}

TEST_CASE("A class does not use itself or its own base", "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    // core/button.h includes widget.h, but Widget is Button's base, so the
    // inherits edge is not duplicated as a uses edge.
    REQUIRE(repo.hasType(QStringLiteral("Button")));
    CHECK(typeNamed(repo, QStringLiteral("Button")).usesLabels.isEmpty());

    // tools/selfref.h includes itself; SelfRef does not use SelfRef.
    REQUIRE(repo.hasType(QStringLiteral("SelfRef")));
    CHECK(typeNamed(repo, QStringLiteral("SelfRef")).usesLabels.isEmpty());
}

TEST_CASE("An include commented out in the source still produces a use",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    // PINNED, NOT ENDORSED. Class and method matching run over text with
    // comments and literals blanked, but include scanning deliberately runs over
    // the RAW text, so `// ... #include "widget.h" ...` still counts. This is
    // what the original scanner did and the move must not silently change it.
    REQUIRE(repo.hasType(QStringLiteral("Commented")));
    CHECK(typeNamed(repo, QStringLiteral("Commented")).usesLabels ==
          QStringList({QStringLiteral("Widget")}));
}

TEST_CASE("Uses are emitted in a stable sorted order without duplicates",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const ir::Repo repo = analyzeRepo(tmp.path());

    // The one deliberate behaviour change in this move: the original computed
    // uses by iterating QHash keys, so the order was hash-dependent and could
    // differ between builds. It is now sorted.
    for (const ir::Type& t : repo.types()) {
        CHECK(t.usesLabels == sortedCopy(t.usesLabels));
        CHECK(QSet<QString>(t.usesLabels.begin(), t.usesLabels.end()).size() ==
              t.usesLabels.size());
    }
}

TEST_CASE("Two analyze runs over the same tree produce identical output",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));

    const ir::Repo first  = analyzeRepo(tmp.path());
    const ir::Repo second = analyzeRepo(tmp.path());

    REQUIRE(first.moduleLabels() == second.moduleLabels());
    REQUIRE(first.typeLabels()   == second.typeLabels());

    for (int i = 0; i < first.modules().size(); ++i) {
        const ir::Module& a = first.modules().at(i);
        const ir::Module& b = second.modules().at(i);
        CHECK(a.description       == b.description);
        CHECK(a.dependsOnLabels   == b.dependsOnLabels);
        CHECK(a.childModuleLabels == b.childModuleLabels);
    }

    for (int i = 0; i < first.types().size(); ++i) {
        const ir::Type& a = first.types().at(i);
        const ir::Type& b = second.types().at(i);
        CHECK(a.description  == b.description);
        CHECK(a.moduleLabel  == b.moduleLabel);
        CHECK(a.baseLabels   == b.baseLabels);
        CHECK(a.usesLabels   == b.usesLabels);
        CHECK(methodLabels(a) == methodLabels(b));
    }
}

// ── Appending ─────────────────────────────────────────────────────────────────

TEST_CASE("Analyze appends into the repo and never clears what is already there",
          "[cppanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    buildSampleRepo(QDir(tmp.path()));
    const RepoFileIndex index = buildRepoFileIndex(tmp.path());

    ir::Repo repo;
    ir::Module preModule;
    preModule.label       = QStringLiteral("preexisting");
    preModule.description = QStringLiteral("Contributed by another analyzer.");
    repo.addModule(preModule);

    ir::Type preType;
    preType.label       = QStringLiteral("PreExisting");
    preType.moduleLabel = QStringLiteral("preexisting");
    repo.addType(preType);

    const CppAnalyzer analyzer;
    QString error;
    REQUIRE(analyzer.analyze(index, &repo, &error));

    // The foreign contribution survives, still first, still intact.
    REQUIRE_FALSE(repo.modules().isEmpty());
    CHECK(repo.modules().first().label == QStringLiteral("preexisting"));
    CHECK(repo.modules().first().description ==
          QStringLiteral("Contributed by another analyzer."));
    REQUIRE_FALSE(repo.types().isEmpty());
    CHECK(repo.types().first().label == QStringLiteral("PreExisting"));
    CHECK(repo.hasModule(QStringLiteral("core")));
    CHECK(repo.hasType(QStringLiteral("Widget")));

    const int modulesAfterFirst = repo.modules().size();
    const int typesAfterFirst   = repo.types().size();

    // A second run appends again; nothing that existed before is removed.
    REQUIRE(analyzer.analyze(index, &repo, &error));
    CHECK(repo.modules().first().label == QStringLiteral("preexisting"));
    CHECK(repo.types().first().label   == QStringLiteral("PreExisting"));
    CHECK(repo.modules().size() >= modulesAfterFirst);
    CHECK(repo.types().size()   >= typesAfterFirst);
    CHECK(repo.hasModule(QStringLiteral("core")));
    CHECK(repo.hasType(QStringLiteral("Widget")));
}

// ── Failure ───────────────────────────────────────────────────────────────────

TEST_CASE("Analyze fails and reports when the tree holds no C++ sources",
          "[cppanalyzer]")
{
    // detect() accepts a tree that only has a CMakeLists.txt, but the scanner
    // itself requires at least one C++ source and refuses otherwise. That is
    // what the original RepoAnalyzer did, and RepoAnalyzer now surfaces this
    // analyzer's error verbatim, so the message has to keep coming from here.
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    writeFile(root.filePath(QStringLiteral("CMakeLists.txt")),
              QStringLiteral("add_library(ghost STATIC)\n"));

    const RepoFileIndex index = buildRepoFileIndex(tmp.path());
    REQUIRE(CppAnalyzer().detect(index));

    ir::Repo repo;
    QString error;
    CHECK_FALSE(CppAnalyzer().analyze(index, &repo, &error));
    CHECK_FALSE(error.isEmpty());
    CHECK(error.contains(QStringLiteral("No C++ source files found under")));

    // A failed run contributes nothing.
    CHECK(repo.isEmpty());

    // A null error pointer is allowed and must not crash.
    ir::Repo other;
    CHECK_FALSE(CppAnalyzer().analyze(index, &other, nullptr));
}
