#include "repofileindex.h"

#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>

#include <utility>

// ── Skip predicate ──────────────────────────────────────────────────────────────
//
// Extracted verbatim from RepoAnalyzer's scanner so the C++ regression contract
// keeps behaving exactly as before. Same cases, same order. Nothing added.
bool isSkippedDir(const QString& name)
{
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return true;
    if (name.startsWith(QLatin1Char('.')))          // .git, .idea, .qt, ...
        return true;
    if (name == QLatin1String("build") ||
        name.startsWith(QLatin1String("cmake-build")) ||
        name == QLatin1String("node_modules") ||
        name.endsWith(QLatin1String("_autogen")) ||
        name == QLatin1String("CMakeFiles"))
        return true;
    return false;
}

// ── The walk ────────────────────────────────────────────────────────────────────
namespace {

// A directory carrying this marker is not source and is skipped whole. Cargo
// writes it into target/; the convention predates Cargo and is language-neutral,
// so no Rust-specific name has to leak into the shared index.
const char kCacheDirTag[] = "CACHEDIR.TAG";

// Depth-first, pre-order, one sorted pass per directory.
//
// Unlike the QDirIterator this replaces, a skipped directory is never opened at
// all: the decision happens on the parent's entry list, before recursion. That
// is what keeps a warm Rust target/ from being enumerated only to be discarded.
void walkDirectory(const QDir& dir, QStringList& out)
{
    // Checked BEFORE reading the contents, so a tagged directory costs one stat
    // rather than a full enumeration.
    if (QFileInfo(dir.filePath(QLatin1String(kCacheDirTag))).isFile())
        return;

    // QDir::Name gives a plain (non locale-aware) name sort, which is what makes
    // the output reproducible run to run. Hidden entries are requested rather
    // than filtered by the platform, so isSkippedDir stays the single authority
    // on what is excluded. An unreadable directory simply yields an empty list.
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks,
        QDir::Name);

    for (const QFileInfo& info : entries) {
        // Applied to files as well as directories. That is a quirk of the
        // original scanner (which matched every path component, file included),
        // and it is preserved deliberately: ".DS_Store" and a stray file named
        // "build" stay out of the index.
        if (isSkippedDir(info.fileName()))
            continue;

        if (info.isDir())
            walkDirectory(QDir(info.absoluteFilePath()), out);
        else if (info.isFile())
            out.append(info.absoluteFilePath());
    }
}

} // namespace

RepoFileIndex buildRepoFileIndex(const QString& rootPath)
{
    if (rootPath.isEmpty())
        return RepoFileIndex();

    const QDir root(rootPath);
    const QString absoluteRoot = root.absolutePath();

    // A missing or unreadable root is not an error here: the index is simply
    // empty, and every analyzer that queries it finds nothing to contribute.
    if (!root.exists())
        return RepoFileIndex(absoluteRoot, QStringList());

    QStringList files;
    walkDirectory(root, files);
    return RepoFileIndex(absoluteRoot, std::move(files));
}

// ── RepoFileIndex ───────────────────────────────────────────────────────────────

RepoFileIndex::RepoFileIndex(QString root, QStringList files)
    : m_root(std::move(root)), m_files(std::move(files))
{
}

QStringList RepoFileIndex::named(const QString& name) const
{
    QStringList result;
    for (const QString& path : m_files)
        if (QFileInfo(path).fileName() == name)
            result.append(path);
    return result;
}

QStringList RepoFileIndex::withSuffix(const QStringList& suffixes) const
{
    QStringList result;
    for (const QString& path : m_files) {
        const QString name = QFileInfo(path).fileName();
        for (const QString& suffix : suffixes) {
            if (name.endsWith(suffix, Qt::CaseInsensitive)) {
                result.append(path);
                break;
            }
        }
    }
    return result;
}

QString RepoFileIndex::relative(const QString& absolutePath) const
{
    if (m_root.isEmpty())
        return absolutePath;
    return QDir(m_root).relativeFilePath(absolutePath);
}
