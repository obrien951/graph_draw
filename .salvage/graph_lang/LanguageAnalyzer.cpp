#include "graph_lang/languageanalyzer.h"

#include <algorithm>
#include <stdexcept>

namespace graph_lang {

// ============================================================================
// Default implementation of LanguageAnalyzer::analyze
// ============================================================================

bool LanguageAnalyzer::analyze(const RepoFileIndex& index,
                                ir::Repo* repo,
                                std::string* error) const {
    // Validate that the index is compatible with this language
    if (!detect(index)) {
        if (error) {
            *error = "Language analyzer '" + id() +
                     "' cannot process the given file index";
        }
        return false;
    }

    // Collect all files and sort them for deterministic ordering
    std::vector<std::string> file_paths;
    file_paths.reserve(index.files.size());

    for (const auto& entry : index.files) {
        file_paths.push_back(entry.path);
    }

    // Sort paths lexicographically to ensure deterministic order
    std::sort(file_paths.begin(), file_paths.end());

    // Analyze each file in sorted order
    for (const auto& path : file_paths) {
        // HARNESS-STUB(RepoFileIndex::get_file_content):
        // Retrieves the content of a file from the index.
        // Declared here, implemented by its own node.
        std::string content = index.get_file_content(path);

        // HARNESS-STUB(Parser::parse):
        // Parses source content into IR nodes.
        // Declared here, implemented by its own node.
        std::vector<ir::Node> nodes = Parser::parse(content, path);

        // HARNESS-STUB(ir::Repo::append_nodes):
        // Appends a batch of IR nodes to the repository.
        // Declared here, implemented by its own node.
        bool success = repo->append_nodes(nodes);

        if (!success) {
            if (error) {
                *error = "Failed to append nodes from file '" + path +
                         "': " + repo->last_error();
            }
            return false;
        }
    }

    return true;
}

} // namespace graph_lang
