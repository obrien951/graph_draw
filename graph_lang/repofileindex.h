#pragma once
#include <QString>
#include <QStringList>

// One filesystem walk, shared by every language analyzer (B1).
//
// Analyzers query this instead of touching the filesystem, which is what makes
// polyglot repositories free: the tree is walked exactly once and each analyzer
// decides from the resulting file list whether it has anything to contribute.
class RepoFileIndex {
public:
    RepoFileIndex() = default;
    RepoFileIndex(QString root, QStringList files);

    // Absolute path of the repository root.
    const QString& root() const { return m_root; }

    // Every non-skipped file, absolute, in deterministic (sorted-per-directory)
    // order.
    const QStringList& files() const { return m_files; }

    bool isEmpty() const { return m_files.isEmpty(); }

    // Absolute paths whose basename is exactly name, e.g. "CMakeLists.txt" or
    // "Cargo.toml".
    QStringList named(const QString& name) const;

    // Absolute paths whose basename ends with any of suffixes, compared
    // case-insensitively, e.g. {".cpp", ".h"} or {".rs"}.
    QStringList withSuffix(const QStringList& suffixes) const;

    // Path relative to root(), for descriptions and labels.
    QString relative(const QString& absolutePath) const;

private:
    QString     m_root;
    QStringList m_files;
};

// Directories the walk never descends into: "." / "..", anything dot-prefixed,
// build, cmake-build*, node_modules, *_autogen and CMakeFiles.
//
// NOTE: callers also apply this to a file's own basename. That quirk is
// inherited from the original scanner and is preserved deliberately.
bool isSkippedDir(const QString& name);

// Walk rootPath, never DESCENDING into a skipped directory, visiting entries in
// sorted order per directory so the result is deterministic.
//
// A directory containing CACHEDIR.TAG is skipped entirely. Cargo writes that
// file into target/, so this kills the "warm target/ holds tens of thousands of
// files" problem language-neutrally, without hardcoding a Rust-specific name.
RepoFileIndex buildRepoFileIndex(const QString& rootPath);
