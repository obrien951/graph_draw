#pragma once
#include <QString>
#include <QList>
#include <QMap>
#include <QSet>

// Forward declarations from graph_lang
namespace ir {
    class Repo;
    class RepoFileIndex;
}

// Forward declaration from graph_core
class GraphScene;

// CppAnalyzer is a stub — declared here, implemented by its own node.
class CppAnalyzer;

/**
 * @brief LanguageAnalyzer implementation for C++ sources.
 *
 * This module wraps the existing C++ scanner behind the LanguageAnalyzer
 * interface. It uses ir::Repo from graph_lang and delegates to CppAnalyzer
 * for the actual analysis. Behavior is preserved byte-for-byte from the
 * existing scanner in graph_analyze/repoanalyzer.cpp.
 */
class LanguageAnalyzer {
public:
    LanguageAnalyzer() = default;

    /**
     * @brief Analyze a repository and build an ir::Repo.
     * @param index The RepoFileIndex from graph_lang.
     * @return An ir::Repo containing the analyzed graph.
     */
    ir::Repo analyze(const ir::RepoFileIndex& index) const;

    /**
     * @brief Build a GraphScene from the analyzed repo.
     * @param repo The ir::Repo from analyze().
     * @param scene The GraphScene to populate.
     * @return true on success.
     */
    bool buildScene(const ir::Repo& repo, GraphScene* scene) const;

    /**
     * @brief Get the last error message.
     */
    QString lastError() const { return m_lastError; }

private:
    QString m_lastError;
};
