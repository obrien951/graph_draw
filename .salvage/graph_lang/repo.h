#ifndef GRAPH_LANG_REPO_H
#define GRAPH_LANG_REPO_H

#include "ir.h"
#include <functional>
#include <string>
#include <vector>

namespace graph_lang {

/**
 * @brief A predicate that determines whether a path should be skipped.
 *
 * The skip predicate is applied to BOTH directories AND files.
 * This is a deliberate design choice: a file named ".gitignore"
 * will be skipped even though it's not a directory.
 *
 * @param path The full path of the entry (directory or file).
 * @param basename The basename of the entry.
 * @return true if this entry should be skipped (not descended into or visited).
 */
using SkipPredicate = std::function<bool(const std::string& path, const std::string& basename)>;

/**
 * @brief Build a deterministic file index from a repository root.
 *
 * This function performs a recursive walk of the repository starting
 * from the given root directory. It never descends into directories
 * that match the skip predicate, and it never visits files that match
 * the skip predicate.
 *
 * The walk visits entries in sorted order per directory to ensure
 * deterministic output across runs and builds.
 *
 * @param root The root directory of the repository.
 * @param skipPredicate A predicate that returns true for entries
 *                      that should be skipped (both directories and files).
 * @param index The RepoFileIndex to populate with discovered files.
 *
 * @note The skip predicate is applied to the file's own basename too,
 *       not just directories. This means a file named ".gitignore"
 *       will be skipped even though it's not a directory.
 *
 * @note This is a recursive walk that never DESCENDS into skipped
 *       directories. If a directory matches the skip predicate,
 *       none of its contents are visited.
 */
void buildRepoFileIndex(
    const std::string& root,
    SkipPredicate skipPredicate,
    RepoFileIndex& index
);

/**
 * @brief Default skip predicate that skips common build artifacts.
 *
 * Skips directories and files matching:
 *   - .git, .svn, .hg, .bzr
 *   - build, dist, out, target, bin, obj
 *   - *.o, *.a, *.so, *.dylib, *.dll
 *   - *.pyc, *.pyo, __pycache__
 *   - node_modules, vendor, .venv, venv
 *   - *.log, *.tmp, *.swp, *.swo
 *   - .DS_Store, Thumbs.db
 *
 * @param path The full path of the entry.
 * @param basename The basename of the entry.
 * @return true if the entry should be skipped.
 */
bool defaultSkipPredicate(const std::string& path, const std::string& basename);

/**
 * @brief Build a file index with the default skip predicate.
 *
 * Convenience wrapper around buildRepoFileIndex that uses the
 * default skip predicate.
 *
 * @param root The root directory of the repository.
 * @param index The RepoFileIndex to populate.
 */
void buildRepoFileIndex(const std::string& root, RepoFileIndex& index);

} // namespace graph_lang

#endif // GRAPH_LANG_REPO_H
