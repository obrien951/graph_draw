#include "repoanalyzer.h"
#include "graphbuilder.h"
#include "cppanalyzer.h"
#include "repofileindex.h"
#include "ir.h"

#include <QDir>
#include <QVector>

bool RepoAnalyzer::analyzeToIr(const QString& repoRoot, ir::Repo* repo)
{
    m_lastError.clear();
    m_languagesUsed.clear();

    if (!repo) {
        m_lastError = QStringLiteral("No IR to analyze into.");
        return false;
    }

    const QDir root(repoRoot);
    if (!root.exists()) {
        m_lastError = QStringLiteral("Repository root does not exist: ") + repoRoot;
        return false;
    }

    // One walk, shared by every analyzer. This is what makes a polyglot repo
    // free: each analyzer decides from the same index whether it has anything to
    // contribute.
    const RepoFileIndex index = buildRepoFileIndex(root.absolutePath());

    CppAnalyzer cpp;
    const QVector<LanguageAnalyzer*> available = { &cpp };

    for (LanguageAnalyzer* analyzer : available) {
        // --lang names one analyzer explicitly; otherwise ask each whether it
        // recognises the tree.
        if (!m_language.isEmpty()) {
            if (analyzer->id() != m_language) continue;
        } else if (!analyzer->detect(index)) {
            continue;
        }

        QString error;
        if (!analyzer->analyze(index, repo, &error)) {
            m_lastError = error;
            return false;
        }
        m_languagesUsed.append(analyzer->id());
    }

    if (m_languagesUsed.isEmpty()) {
        m_lastError = m_language.isEmpty()
            ? QStringLiteral("No supported source files found under ") + root.absolutePath()
            : QStringLiteral("No analyzer with id \"%1\".").arg(m_language);
        return false;
    }
    return true;
}

bool RepoAnalyzer::analyze(const QString& repoRoot, GraphScene* scene)
{
    ir::Repo repo;
    if (!analyzeToIr(repoRoot, &repo))
        return false;

    GraphBuilder().build(repo, scene);
    return true;
}
