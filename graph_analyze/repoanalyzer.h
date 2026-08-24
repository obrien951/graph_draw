#pragma once
#include <QString>
#include <QStringList>

class GraphScene;
namespace ir { class Repo; }

// RepoAnalyzer scans a repository and builds a module/class/function dependency
// graph into a GraphScene, in the same shape as graphs/graph_draw.json.
//
// Since Part B it is a DISPATCHER, not a scanner. The work is split three ways:
//   • RepoFileIndex (graph_lang)   walks the tree exactly once
//   • a LanguageAnalyzer per language appends into a shared ir::Repo
//   • GraphBuilder                 turns that IR into scene items
// so adding a language means adding a plugin, not editing this class.
//
// Language plugins live in their own libraries (graph_lang_cpp, graph_lang_rust)
// and link Qt Core only — they cannot see GraphScene, which is what lets their
// tests run without a QApplication.
//
// Edges follow the graph_draw convention "A → B means A depends on B":
//   module → module     ("contains" for nesting, "depends on" for linkage)
//   module → class      ("contains")
//   class  → function   ("provides")
//   class  → base class ("inherits")
//   class  → class      ("uses")
//
// Every discovered node is marked implemented (the code already exists). The
// scanners are heuristic — no libclang, no compiler — so templates, macros and
// other exotic constructs may be missed; results are a faithful-enough starting
// graph, not a proof.
class RepoAnalyzer {
public:
    RepoAnalyzer() = default;

    // Populate scene with the graph for the repository rooted at repoRoot.
    // Any existing scene contents are cleared first. Returns true on success.
    bool analyze(const QString& repoRoot, GraphScene* scene);

    // Scene-free entry point, for tests and for tools that want the raw model
    // without a QApplication. Appends into repo.
    bool analyzeToIr(const QString& repoRoot, ir::Repo* repo);

    // Restrict analysis to one language id ("cpp", "rust"); empty means
    // autodetect. The escape hatch for polyglot repos where labels collide.
    void setLanguage(const QString& languageId) { m_language = languageId; }
    QString language() const { return m_language; }

    // Ids of the analyzers that actually contributed, in run order. Populated by
    // the last analyze()/analyzeToIr() call.
    QStringList languagesUsed() const { return m_languagesUsed; }

    // Human-readable description of the last error, or empty if none.
    QString lastError() const { return m_lastError; }

private:
    QString     m_lastError;
    QString     m_language;        // empty = autodetect
    QStringList m_languagesUsed;
};
