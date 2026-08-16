#pragma once
#include <QString>

class GraphScene;

// RepoAnalyzer scans a C++ repository and builds a module/class/function
// dependency graph into a GraphScene, in the same shape as graphs/graph_draw.json.
//
// It is a self-contained heuristic scanner — no libclang, no compiler needed:
//   • Modules   come from CMake add_library / add_executable targets (a
//               directory fallback is used for sources not owned by any target).
//   • Classes   come from `class` / `struct` definitions in the sources.
//   • Functions come from member functions declared in those classes.
//
// Edges follow the graph_draw convention "A → B means A depends on B":
//   module → class      ("contains")
//   class  → function   ("provides")
//   class  → base class ("inherits")
//   class  → class      ("uses", inferred from #include)
//   module → module     ("depends on", from target_link_libraries)
//
// Every discovered node is marked implemented (the code already exists). Because
// the scanner is heuristic, templates, macros, and other exotic constructs may
// be missed; results are meant as a faithful-enough starting graph, not a proof.
class RepoAnalyzer {
public:
    RepoAnalyzer() = default;

    // Populate scene with the graph for the repository rooted at repoRoot.
    // Any existing scene contents are cleared first. Returns true on success.
    bool analyze(const QString& repoRoot, GraphScene* scene);

    // Human-readable description of the last error, or empty if none.
    QString lastError() const { return m_lastError; }

private:
    QString m_lastError;
};
