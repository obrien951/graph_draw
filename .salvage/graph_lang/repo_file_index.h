#ifndef GRAPH_LANG_REPO_FILE_INDEX_H
#define GRAPH_LANG_REPO_FILE_INDEX_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QDir>

namespace graph_lang {

/**
 * @brief A filesystem index that walks the repository and removes build/VCS noise.
 *
 * Performs a single filesystem walk that does NOT descend into skipped
 * directories (unlike QDirIterator which descends and filters afterwards).
 * This is more efficient for C++ where directory traversal is expensive.
 */
class RepoFileIndex {
public:
    /**
     * @brief Constructs an index for the given root directory.
     * @param root The root directory to index (absolute path).
     * @param skipDirs Directories to skip during the walk (e.g., .git, build, out).
     */
    explicit RepoFileIndex(const QString& root, const QStringList& skipDirs = QStringList());

    /**
     * @brief Static factory: returns absolute paths whose basename ends with
     *        any of the given suffixes, case-insensitively.
     *
     * This performs a single filesystem walk and filters results in one pass.
     *
     * @param suffixes List of suffixes to match (e.g. {".cpp", ".h"}).
     * @param root The root directory to search (defaults to current working directory).
     * @return QVector of absolute paths whose basename ends with any suffix.
     */
    static QVector<QString> withSuffix(const QStringList& suffixes, const QString& root = QString());

    /**
     * @brief Returns all files in the index (absolute paths).
     */
    QVector<QString> allFiles() const;

    /**
     * @brief Returns files matching the given suffixes (case-insensitive).
     */
    QVector<QString> filesWithSuffix(const QStringList& suffixes) const;

    /**
     * @brief Returns the root directory of the index.
     */
    QString root() const { return root_; }

    /**
     * @brief Returns the list of directories that were skipped during the walk.
     */
    QStringList skippedDirs() const { return skippedDirs_; }

private:
    QString root_;
    QStringList skippedDirs_;
    QVector<QString> files_;

    /**
     * @brief Recursively walks the directory tree, skipping specified directories.
     * @param dir The directory to walk.
     * @param skipDirs Directories to skip.
     * @param files Output list of absolute file paths.
     */
    void walkDir(const QString& dir, const QStringList& skipDirs, QVector<QString>& files);
};

} // namespace graph_lang

#endif // GRAPH_LANG_REPO_FILE_INDEX_H
