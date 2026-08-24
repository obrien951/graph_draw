#pragma once

#include <string>
#include <vector>
#include <memory>

#include "graph_core/graphnode.h"
#include "graph_core/graphedge.h"
#include "graph_io/graphserializer.h"

namespace graph_lang_rust {

/**
 * @brief Analyzes a Rust workspace and produces a graph representation.
 * 
 * Discovers crates from Cargo.toml, derives depth-1 nested modules from
 * the filesystem, scans items, and resolves edges.
 * 
 * Crate label is package.name — the same name that appears in use statements
 * and other crate references.
 */
class RustAnalyzer {
public:
    /**
     * @brief Analyzes the given repository index and returns a Repo.
     * 
     * @param index The file index of the repository.
     * @return ir::Repo The analyzed repository graph.
     */
    ir::Repo analyze(const RepoFileIndex& index) const;

private:
    /**
     * @brief Discovers crates from Cargo.toml files in the workspace.
     * 
     * @param index The file index.
     * @return std::vector<CrateInfo> List of discovered crates.
     */
    std::vector<CrateInfo> discover_crates(const RepoFileIndex& index) const;

    /**
     * @brief Derives depth-1 nested modules from src/ paths.
     * 
     * Nested modules are depth-1 and derived from src/ paths,
     * so no mod resolution is needed and the tree cannot disagree with itself.
     * 
     * @param crate_info Information about a crate.
     * @return std::vector<ModuleInfo> List of derived modules.
     */
    std::vector<ModuleInfo> derive_modules(const CrateInfo& crate_info) const;

    /**
     * @brief Scans items in a module and resolves edges.
     * 
     * @param module_info Information about a module.
     * @param index The file index.
     * @return std::vector<GraphNode> List of graph nodes.
     */
    std::vector<GraphNode> scan_items(const ModuleInfo& module_info,
                                      const RepoFileIndex& index) const;

    /**
     * @brief Resolves edges between nodes based on use statements and imports.
     * 
     * @param nodes The nodes to resolve edges between.
     * @param index The file index.
     * @return std::vector<GraphEdge> List of resolved edges.
     */
    std::vector<GraphEdge> resolve_edges(const std::vector<GraphNode>& nodes,
                                         const RepoFileIndex& index) const;

    /**
     * @brief Represents a discovered crate from Cargo.toml.
     */
    struct CrateInfo {
        std::string name;           // package.name, used as crate label
        std::string version;        // package.version
        std::string path;           // path to the crate root
        std::string manifest_path;  // path to Cargo.toml
        std::vector<std::string> members;  // [[bin]] or [lib] members
    };

    /**
     * @brief Represents a derived module from a crate.
     */
    struct ModuleInfo {
        std::string name;           // module name
        std::string path;           // filesystem path
        std::string crate_name;     // parent crate name
        std::string module_path;    // full module path
    };

    /**
     * @brief Represents a scanned item (struct, enum, function, etc.).
     */
    struct ItemInfo {
        std::string name;
        std::string kind;           // "struct", "enum", "fn", "mod", etc.
        std::string module_path;
        std::string visibility;     // "pub", "pub(crate)", etc.
    };
};

} // namespace graph_lang_rust

