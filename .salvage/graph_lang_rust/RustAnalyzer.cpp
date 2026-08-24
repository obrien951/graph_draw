#include "graph_lang_rust/rustanalyzer.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>
#include <set>
#include <sstream>

#include "graph_core/graphnode.h"
#include "graph_core/graphedge.h"

namespace fs = std::filesystem;

namespace graph_lang_rust {

ir::Repo RustAnalyzer::analyze(const RepoFileIndex& index) const {
    // Step 1: Discover crates from Cargo.toml files
    std::vector<CrateInfo> crates = discover_crates(index);

    // Step 2: Derive depth-1 nested modules from src/ paths
    std::vector<ModuleInfo> all_modules;
    for (const auto& crate : crates) {
        std::vector<ModuleInfo> modules = derive_modules(crate);
        all_modules.insert(all_modules.end(), std::make_move_iterator(modules.begin()),
                          std::make_move_iterator(modules.end()));
    }

    // Step 3: Scan items in each module
    std::vector<GraphNode> nodes;
    for (const auto& module : all_modules) {
        std::vector<GraphNode> module_nodes = scan_items(module, index);
        nodes.insert(nodes.end(), std::make_move_iterator(module_nodes.begin()),
                     std::make_move_iterator(module_nodes.end()));
    }

    // Step 4: Resolve edges between nodes
    std::vector<GraphEdge> edges = resolve_edges(nodes, index);

    // Build and return the Repo
    return ir::Repo{
        .nodes = std::move(nodes),
        .edges = std::move(edges),
        .modules = std::move(all_modules),
        .crates = std::move(crates),
    };
}

std::vector<CrateInfo> RustAnalyzer::discover_crates(const RepoFileIndex& index) const {
    std::vector<CrateInfo> crates;
    std::set<std::string> seen_paths;

    // Find all Cargo.toml files in the workspace
    for (const auto& [path, content] : index.files()) {
        if (path.ends_with("/Cargo.toml")) {
            if (seen_paths.count(path)) continue;
            seen_paths.insert(path);

            CrateInfo crate;
            crate.manifest_path = path;

            // Parse Cargo.toml to extract package.name and members
            std::string manifest = content;
            std::regex name_regex(R"(name\s*=\s*["']([^"']+)["'])");
            std::smatch match;
            if (std::regex_search(manifest, match, name_regex)) {
                crate.name = match[1].str();
            }

            std::regex version_regex(R"(version\s*=\s*["']([^"']+)["'])");
            if (std::regex_search(manifest, match, version_regex)) {
                crate.version = match[1].str();
            }

            // Extract members (both [lib] and [[bin]])
            std::regex members_regex(R"(members\s*=\s*\[([^\]]+)\])");
            if (std::regex_search(manifest, match, members_regex)) {
                std::string members_str = match[1].str();
                std::regex member_regex(R"(\s*["']([^"']+)["']\s*)");
                std::sregex_iterator it(members_str.begin(), members_str.end(), member_regex);
                std::sregex_iterator end;
                while (it != end) {
                    crate.members.push_back(it->str(1));
                    ++it;
                }
            }

            crates.push_back(std::move(crate));
        }
    }

    // Sort by crate name
    std::sort(crates.begin(), crates.end(),
              [](const CrateInfo& a, const CrateInfo& b) {
                  return a.name < b.name;
              });

    return crates;
}

std::vector<ModuleInfo> RustAnalyzer::derive_modules(const CrateInfo& crate) const {
    std::vector<ModuleInfo> modules;

    // The crate root module is always derived from the crate name
    ModuleInfo root_module;
    root_module.name = crate.name;
    root_module.path = crate.path;
    root_module.crate_name = crate.name;
    root_module.module_path = crate.name;
    modules.push_back(std::move(root_module));

    // Derive depth-1 nested modules from src/ paths
    // Nested modules are depth-1 and derived from src/ paths,
    // so no mod resolution is needed and the tree cannot disagree with itself.
    for (const auto& member : crate.members) {
        std::string src_path = fs::path(crate.path) / "src" / member;
        if (fs::exists(src_path)) {
            ModuleInfo module;
            module.name = member;
            module.path = src_path;
            module.crate_name = crate.name;
            module.module_path = crate.name + "::" + member;
            modules.push_back(std::move(module));
        }
    }

    return modules;
}

std::vector<GraphNode> RustAnalyzer::scan_items(const ModuleInfo& module,
                                                const RepoFileIndex& index) const {
    std::vector<GraphNode> nodes;

    // Read the module's source file
    std::string content;
    if (auto it = index.files().find(module.path); it != index.files().end()) {
        content = it->second;
    }

    // Scan for items: structs, enums, functions, modules
    // This is a simplified scanner that identifies item declarations
    std::regex struct_regex(R"(pub\s+struct\s+(\w+))");
    std::regex enum_regex(R"(pub\s+enum\s+(\w+))");
    std::regex fn_regex(R"(pub\s+fn\s+(\w+)\s*\()");
    std::regex mod_regex(R"(pub\s+mod\s+(\w+))");

    std::sregex_iterator it(content.begin(), content.end(), struct_regex);
    std::sregex_iterator end;
    while (it != end) {
        ItemInfo item;
        item.name = it->str(1);
        item.kind = "struct";
        item.module_path = module.module_path;
        item.visibility = "pub";
        nodes.push_back(GraphNode{
            .name = item.name,
            .kind = item.kind,
            .module_path = item.module_path,
        });
        ++it;
    }

    it = std::sregex_iterator(content.begin(), content.end(), enum_regex);
    end = std::sregex_iterator();
    while (it != end) {
        ItemInfo item;
        item.name = it->str(1);
        item.kind = "enum";
        item.module_path = module.module_path;
        item.visibility = "pub";
        nodes.push_back(GraphNode{
            .name = item.name,
            .kind = item.kind,
            .module_path = item.module_path,
        });
        ++it;
    }

    it = std::sregex_iterator(content.begin(), content.end(), fn_regex);
    end = std::sregex_iterator();
    while (it != end) {
        ItemInfo item;
        item.name = it->str(1);
        item.kind = "fn";
        item.module_path = module.module_path;
        item.visibility = "pub";
        nodes.push_back(GraphNode{
            .name = item.name,
            .kind = item.kind,
            .module_path = item.module_path,
        });
        ++it;
    }

    it = std::sregex_iterator(content.begin(), content.end(), mod_regex);
    end = std::sregex_iterator();
    while (it != end) {
        ItemInfo item;
        item.name = it->str(1);
        item.kind = "mod";
        item.module_path = module.module_path;
        item.visibility = "pub";
        nodes.push_back(GraphNode{
            .name = item.name,
            .kind = item.kind,
            .module_path = item.module_path,
        });
        ++it;
    }

    return nodes;
}

std::vector<GraphEdge> RustAnalyzer::resolve_edges(const std::vector<GraphNode>& nodes,
                                                   const RepoFileIndex& index) const {
    std::vector<GraphEdge> edges;

    // For each node, scan its source file for use statements and imports
    for (const auto& node : nodes) {
        std::string content;
        if (auto it = index.files().find(node.module_path); it != index.files().end()) {
            content = it->second;
        }

        // Find use statements: use crate::Name or use module::Name
        std::regex use_regex(R"(use\s+(?:crate::)?(\w+::)?(\w+))");
        std::sregex_iterator it(content.begin(), content.end(), use_regex);
        std::sregex_iterator end;

        while (it != end) {
            std::string prefix = it->str(1);
            std::string name = it->str(2);

            // Check if the referenced item exists in our node set
            for (const auto& target : nodes) {
                if (target.name == name) {
                    // Create an edge from the current node to the target
                    edges.push_back(GraphEdge{
                        .source = node.name,
                        .target = target.name,
                        .kind = "use",
                    });
                    break;
                }
            }
            ++it;
        }
    }

    return edges;
}

} // namespace graph_lang_rust

