#include <catch2/catch_test_macros.hpp>

#include "repofileindex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>

// ── Helpers ───────────────────────────────────────────────────────────────────

static void writeFile(const QString& path, const QString& contents)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(contents.toUtf8());
    f.close();
}

// A repository with the shapes that matter to the walk: nested sources, the
// directories the skip predicate rejects, and a Cargo-style target/ marked with
// CACHEDIR.TAG.
static void buildSampleRepo(const QDir& root)
{
    root.mkpath(QStringLiteral("src/core"));
    root.mkpath(QStringLiteral("src/legacy"));
    root.mkpath(QStringLiteral("src/sorted"));
    root.mkpath(QStringLiteral("src/foo_autogen"));
    root.mkpath(QStringLiteral(".git/refs"));
    root.mkpath(QStringLiteral("build/CMakeFiles"));
    root.mkpath(QStringLiteral("cmake-build-debug"));
    root.mkpath(QStringLiteral("node_modules/left-pad"));
    root.mkpath(QStringLiteral("CMakeFiles"));
    root.mkpath(QStringLiteral("target/debug/deps"));

    // ── Real sources ──────────────────────────────────────────────────────────
    writeFile(root.filePath("CMakeLists.txt"),
              "add_subdirectory(src)\n");
    writeFile(root.filePath("src/CMakeLists.txt"),
              "add_library(core STATIC core/widget.cpp)\n");
    writeFile(root.filePath("src/main.cpp"),        "int main() { return 0; }\n");
    writeFile(root.filePath("src/core/widget.h"),   "#pragma once\nclass Widget {};\n");
    writeFile(root.filePath("src/core/widget.cpp"), "#include \"widget.h\"\n");
    writeFile(root.filePath("src/core/notes.txt"),  "not a source file\n");

    // Uppercase extension: withSuffix must match it case-insensitively.
    writeFile(root.filePath("src/legacy/Legacy.CPP"), "// old style\n");

    // All-lowercase names in one leaf directory, so the expected order is the
    // same whether the platform compares case-sensitively or not.
    writeFile(root.filePath("src/sorted/alpha.cpp"), "// a\n");
    writeFile(root.filePath("src/sorted/beta.cpp"),  "// b\n");
    writeFile(root.filePath("src/sorted/gamma.cpp"), "// c\n");

    // ── Files whose own basename hits the skip predicate ──────────────────────
    // Inherited quirk: the predicate is applied to files as well as directories.
    writeFile(root.filePath("src/core/.DS_Store"), "junk\n");
    writeFile(root.filePath("src/core/build"),     "a FILE named build\n");

    // ── Directories that must never be descended into ─────────────────────────
    writeFile(root.filePath(".git/HEAD"),                    "ref: refs/heads/main\n");
    writeFile(root.filePath(".git/refs/hidden.cpp"),         "// must not appear\n");
    writeFile(root.filePath("build/generated.cpp"),          "// must not appear\n");
    writeFile(root.filePath("build/CMakeLists.txt"),         "# must not appear\n");
    writeFile(root.filePath("build/CMakeFiles/link.txt"),    "# must not appear\n");
    writeFile(root.filePath("cmake-build-debug/stale.cpp"),  "// must not appear\n");
    writeFile(root.filePath("node_modules/left-pad/index.js"), "// must not appear\n");
    writeFile(root.filePath("src/foo_autogen/moc_widget.cpp"), "// must not appear\n");
    writeFile(root.filePath("CMakeFiles/TargetDirectories.txt"), "# must not appear\n");

    // ── The Rust problem: a warm target/, marked as a cache directory ─────────
    writeFile(root.filePath("target/CACHEDIR.TAG"),
              "Signature: 8a477f597d28d172789f06886806bc55\n");
    writeFile(root.filePath("target/decoy.rs"),           "// must not appear\n");
    writeFile(root.filePath("target/debug/deps/deep.rs"), "// must not appear\n");
}

static QStringList relatives(const RepoFileIndex& index)
{
    QStringList out;
    for (const QString& path : index.files())
        out.append(index.relative(path));
    return out;
}

static bool containsRelative(const RepoFileIndex& index, const QString& rel)
{
    return relatives(index).contains(rel);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("isSkippedDir rejects exactly the documented names", "[repofileindex]")
{
    REQUIRE(isSkippedDir(QStringLiteral(".")));
    REQUIRE(isSkippedDir(QStringLiteral("..")));
    REQUIRE(isSkippedDir(QStringLiteral(".git")));
    REQUIRE(isSkippedDir(QStringLiteral(".idea")));
    REQUIRE(isSkippedDir(QStringLiteral(".DS_Store")));
    REQUIRE(isSkippedDir(QStringLiteral("build")));
    REQUIRE(isSkippedDir(QStringLiteral("cmake-build")));
    REQUIRE(isSkippedDir(QStringLiteral("cmake-build-debug")));
    REQUIRE(isSkippedDir(QStringLiteral("node_modules")));
    REQUIRE(isSkippedDir(QStringLiteral("foo_autogen")));
    REQUIRE(isSkippedDir(QStringLiteral("CMakeFiles")));

    // Ordinary names survive, including near-misses.
    REQUIRE_FALSE(isSkippedDir(QStringLiteral("src")));
    REQUIRE_FALSE(isSkippedDir(QStringLiteral("core")));
    REQUIRE_FALSE(isSkippedDir(QStringLiteral("target")));
    REQUIRE_FALSE(isSkippedDir(QStringLiteral("builder")));
    REQUIRE_FALSE(isSkippedDir(QStringLiteral("rebuild")));
    REQUIRE_FALSE(isSkippedDir(QStringLiteral("CMakeLists.txt")));
    REQUIRE_FALSE(isSkippedDir(QStringLiteral("widget.cpp")));
}

TEST_CASE("The walk collects real sources with absolute paths", "[repofileindex]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    const RepoFileIndex index = buildRepoFileIndex(root.path());

    REQUIRE_FALSE(index.isEmpty());
    REQUIRE(index.root() == root.absolutePath());
    for (const QString& path : index.files())
        REQUIRE(QFileInfo(path).isAbsolute());

    REQUIRE(containsRelative(index, QStringLiteral("CMakeLists.txt")));
    REQUIRE(containsRelative(index, QStringLiteral("src/CMakeLists.txt")));
    REQUIRE(containsRelative(index, QStringLiteral("src/main.cpp")));
    REQUIRE(containsRelative(index, QStringLiteral("src/core/widget.h")));
    REQUIRE(containsRelative(index, QStringLiteral("src/core/widget.cpp")));
    REQUIRE(containsRelative(index, QStringLiteral("src/core/notes.txt")));
    REQUIRE(containsRelative(index, QStringLiteral("src/legacy/Legacy.CPP")));
}

TEST_CASE("Skipped directories and their contents never appear", "[repofileindex]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    const RepoFileIndex index = buildRepoFileIndex(root.path());
    const QStringList rels = relatives(index);

    // No component of any indexed path may be a skipped name.
    for (const QString& rel : rels) {
        for (const QString& part : rel.split(QLatin1Char('/'))) {
            REQUIRE_FALSE(isSkippedDir(part));
        }
    }

    REQUIRE_FALSE(rels.contains(QStringLiteral(".git/HEAD")));
    REQUIRE_FALSE(rels.contains(QStringLiteral(".git/refs/hidden.cpp")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("build/generated.cpp")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("build/CMakeLists.txt")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("build/CMakeFiles/link.txt")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("cmake-build-debug/stale.cpp")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("node_modules/left-pad/index.js")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("src/foo_autogen/moc_widget.cpp")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("CMakeFiles/TargetDirectories.txt")));
}

TEST_CASE("The skip predicate applies to a file's own basename", "[repofileindex]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    const RepoFileIndex index = buildRepoFileIndex(root.path());
    const QStringList rels = relatives(index);

    // Both are ordinary files living in a directory that IS walked.
    REQUIRE(QFileInfo(root.filePath("src/core/.DS_Store")).isFile());
    REQUIRE(QFileInfo(root.filePath("src/core/build")).isFile());

    REQUIRE_FALSE(rels.contains(QStringLiteral("src/core/.DS_Store")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("src/core/build")));
}

TEST_CASE("A directory holding CACHEDIR.TAG is skipped whole", "[repofileindex]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    const RepoFileIndex index = buildRepoFileIndex(root.path());
    const QStringList rels = relatives(index);

    // target/ is not a skipped NAME, so only the tag can be keeping it out.
    REQUIRE_FALSE(isSkippedDir(QStringLiteral("target")));
    REQUIRE(QFileInfo(root.filePath("target/decoy.rs")).isFile());

    REQUIRE_FALSE(rels.contains(QStringLiteral("target/decoy.rs")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("target/debug/deps/deep.rs")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("target/CACHEDIR.TAG")));

    for (const QString& rel : rels)
        REQUIRE_FALSE(rel.startsWith(QStringLiteral("target/")));

    // The tag is what does it: without one, an identically shaped directory is
    // walked normally.
    REQUIRE(root.mkpath(QStringLiteral("untagged")));
    writeFile(root.filePath("untagged/kept.rs"), "// kept\n");
    const RepoFileIndex again = buildRepoFileIndex(root.path());
    REQUIRE(relatives(again).contains(QStringLiteral("untagged/kept.rs")));
}

TEST_CASE("The walk is deterministic and sorted within each directory", "[repofileindex]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    const RepoFileIndex first  = buildRepoFileIndex(root.path());
    const RepoFileIndex second = buildRepoFileIndex(root.path());

    REQUIRE(first.files() == second.files());
    REQUIRE(first.root()  == second.root());

    const QStringList rels = relatives(first);
    const int alpha = rels.indexOf(QStringLiteral("src/sorted/alpha.cpp"));
    const int beta  = rels.indexOf(QStringLiteral("src/sorted/beta.cpp"));
    const int gamma = rels.indexOf(QStringLiteral("src/sorted/gamma.cpp"));
    REQUIRE(alpha >= 0);
    REQUIRE(alpha < beta);
    REQUIRE(beta  < gamma);

    // Siblings in src/core come out sorted too.
    const int notes  = rels.indexOf(QStringLiteral("src/core/notes.txt"));
    const int wcpp   = rels.indexOf(QStringLiteral("src/core/widget.cpp"));
    const int wh     = rels.indexOf(QStringLiteral("src/core/widget.h"));
    REQUIRE(notes >= 0);
    REQUIRE(notes < wcpp);
    REQUIRE(wcpp  < wh);

    // No duplicates, whatever the order.
    QStringList unique = first.files();
    unique.sort();
    unique.removeDuplicates();
    REQUIRE(unique.size() == first.files().size());
}

TEST_CASE("named() matches basenames exactly", "[repofileindex]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    const RepoFileIndex index = buildRepoFileIndex(root.path());

    const QStringList cmakeLists = index.named(QStringLiteral("CMakeLists.txt"));
    REQUIRE(cmakeLists.size() == 2);   // root and src; the one under build/ is gone
    QStringList rels;
    for (const QString& path : cmakeLists)
        rels.append(index.relative(path));
    REQUIRE(rels.contains(QStringLiteral("CMakeLists.txt")));
    REQUIRE(rels.contains(QStringLiteral("src/CMakeLists.txt")));

    // Exact match only: no partial names, no case folding.
    REQUIRE(index.named(QStringLiteral("CMakeLists")).isEmpty());
    REQUIRE(index.named(QStringLiteral("cmakelists.txt")).isEmpty());
    REQUIRE(index.named(QStringLiteral("Cargo.toml")).isEmpty());

    // Order of files() is preserved.
    QList<int> positions;
    for (const QString& path : cmakeLists)
        positions.append(index.files().indexOf(path));
    REQUIRE(positions.size() == 2);
    REQUIRE(positions.at(0) < positions.at(1));
}

TEST_CASE("withSuffix() matches case-insensitively and keeps order", "[repofileindex]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    const RepoFileIndex index = buildRepoFileIndex(root.path());

    const QStringList sources =
        index.withSuffix(QStringList{QStringLiteral(".cpp"), QStringLiteral(".h")});

    QStringList rels;
    for (const QString& path : sources)
        rels.append(index.relative(path));

    REQUIRE(rels.contains(QStringLiteral("src/main.cpp")));
    REQUIRE(rels.contains(QStringLiteral("src/core/widget.cpp")));
    REQUIRE(rels.contains(QStringLiteral("src/core/widget.h")));
    REQUIRE(rels.contains(QStringLiteral("src/sorted/alpha.cpp")));
    // The uppercase extension is the point of this case.
    REQUIRE(rels.contains(QStringLiteral("src/legacy/Legacy.CPP")));

    REQUIRE_FALSE(rels.contains(QStringLiteral("src/core/notes.txt")));
    REQUIRE_FALSE(rels.contains(QStringLiteral("CMakeLists.txt")));

    REQUIRE(index.withSuffix(QStringList{QStringLiteral(".rs")}).isEmpty());
    REQUIRE(index.withSuffix(QStringList()).isEmpty());

    // The result is a subsequence of files(), in the same order.
    int previous = -1;
    for (const QString& path : sources) {
        const int at = index.files().indexOf(path);
        REQUIRE(at > previous);
        previous = at;
    }
}

TEST_CASE("relative() round-trips against root()", "[repofileindex]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleRepo(root);

    const RepoFileIndex index = buildRepoFileIndex(root.path());
    const QDir indexRoot(index.root());

    for (const QString& path : index.files()) {
        const QString rel = index.relative(path);
        REQUIRE_FALSE(rel.startsWith(QLatin1Char('/')));
        REQUIRE_FALSE(rel.startsWith(QStringLiteral("..")));
        REQUIRE(indexRoot.absoluteFilePath(rel) == path);
    }
}

TEST_CASE("A nonexistent root yields an empty index", "[repofileindex]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());

    const RepoFileIndex missing =
        buildRepoFileIndex(root.filePath(QStringLiteral("no-such-directory")));
    REQUIRE(missing.isEmpty());
    REQUIRE(missing.files().isEmpty());
    REQUIRE(missing.named(QStringLiteral("CMakeLists.txt")).isEmpty());
    REQUIRE(missing.withSuffix(QStringList{QStringLiteral(".cpp")}).isEmpty());

    REQUIRE(buildRepoFileIndex(QString()).isEmpty());

    // An existing but empty directory is empty too, and still knows its root.
    REQUIRE(root.mkpath(QStringLiteral("empty")));
    const RepoFileIndex blank = buildRepoFileIndex(root.filePath(QStringLiteral("empty")));
    REQUIRE(blank.isEmpty());
    REQUIRE(blank.root() == QDir(root.filePath(QStringLiteral("empty"))).absolutePath());

    // A default-constructed index is empty as well.
    REQUIRE(RepoFileIndex().isEmpty());
}
