#ifndef GRAPH_LANG_LANGUAGEANALYZER_H
#define GRAPH_LANG_LANGUAGEANALYZER_H

#include <string>
#include <memory>
#include <vector>

namespace graph_lang {

// Forward declarations for dependencies that do not exist yet.
// These are stubbed by the harness; their implementations are provided
// by other agents in the graph.
class ir::Repo;
struct RepoFileIndex;

/**
 * @brief Abstract base class for language-specific analyzers.
 *
 * Every language plugin implements this interface to contribute nodes
 * to the IR repository. The analyzer is responsible for:
 *   - Detecting the language in a file index
 *   - Parsing source files into IR nodes
 *   - Appending nodes to the repository in a deterministic order
 *
 * The deterministic ordering requirement ensures reproducible builds
 * and consistent graph traversal regardless of filesystem ordering.
 */
class LanguageAnalyzer {
public:
    virtual ~LanguageAnalyzer() = default;

    /**
     * @brief Returns the unique identifier for this language analyzer.
     * @return The language ID string (e.g., "cpp", "rust", "python").
     */
    virtual std::string id() const = 0;

    /**
     * @brief Detects whether this analyzer can handle the given file index.
     *
     * @param index The prebuilt file index from the repository.
     * @return true if this analyzer can process the index, false otherwise.
     */
    virtual bool detect(const RepoFileIndex& index) const = 0;

    /**
     * @brief Analyzes the file index and appends IR nodes to the repository.
     *
     * This is the main entry point for language analysis. It must:
     *   1. Validate the index is compatible with this language
     *   2. Parse each file in deterministic order (sorted by path)
     *   3. Create IR nodes for each parsed entity
     *   4. Append nodes to the provided repository
     *   5. Return false and set an error on any failure
     *
     * @param index The prebuilt file index to analyze.
     * @param repo The repository to append nodes into.
     * @param error Pointer to an error object to set on failure.
     * @return true on success, false on failure with error set.
     */
    virtual bool analyze(const RepoFileIndex& index,
                         ir::Repo* repo,
                         std::string* error) const = 0;
};

} // namespace graph_lang

#endif // GRAPH_LANG_LANGUAGEANALYZER_H
