#pragma once

#include <QString>
#include <QStringList>
#include <QDir>

namespace graph_lang {

/**
 * @brief A filesystem index that provides absolute paths for files by basename.
 * 
 * Walks the filesystem once and caches results, avoiding redundant traversals.
 * Skips build artifacts, VCS directories, and other noise.
 */
class RepoFileIndex {
public:
    /**
     * @brief Constructs an index for the given root directory.
     * @param root The root directory to index (must be within graph_lang/**).
     */
    explicit RepoFileIndex(const QString& root);

    /**
     * @brief Returns all absolute paths whose basename is exactly the given name.
     * @param name The exact basename to search for (e.g., "CMakeLists.txt").
     * @return List of absolute paths matching the name.
     */
    QStringList named(const QString& name) const;

    /**
     * @brief Returns all absolute paths found in the index.
     * @return List of all absolute paths.
     */
    QStringList all() const;

    /**
     * @brief Returns the root directory of the index.
     * @return The root directory path.
     */
    QString root() const { return m_root; }

private:
    QString m_root;
    mutable QStringList m_paths;  // Cached absolute paths
    mutable bool m_indexed = false;

    void index() const;
};

} // namespace graph_lang
