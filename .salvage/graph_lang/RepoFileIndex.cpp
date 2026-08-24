#include "RepoFileIndex.h"
#include <QDir>
#include <QFileInfo>

namespace graph_lang {

RepoFileIndex::RepoFileIndex(const QString& root)
    : m_root(root)
{
}

void RepoFileIndex::index() const
{
    if (m_indexed) {
        return;
    }

    QDir dir(m_root);
    dir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);

    for (const QFileInfo& info : dir.entryInfoList()) {
        if (info.isDir()) {
            // Skip VCS and build directories — do not descend into them
            if (info.fileName() == ".git" ||
                info.fileName() == ".harness" ||
                info.fileName() == "agent_harness" ||
                info.fileName() == "graph_agent.py") {
                continue;
            }
            // Skip any other directory that looks like noise
            if (info.fileName().startsWith(".") ||
                info.fileName().startsWith("_") ||
                info.fileName() == "build" ||
                info.fileName() == "dist" ||
                info.fileName() == "out" ||
                info.fileName() == "bin" ||
                info.fileName() == "obj" ||
                info.fileName() == "target" ||
                info.fileName() == "venv" ||
                info.fileName() == ".venv" ||
                info.fileName() == "__pycache__") {
                continue;
            }
            // Recurse into non-skipped directories
            index(info.absoluteFilePath());
        } else {
            m_paths.append(info.absoluteFilePath());
        }
    }

    m_indexed = true;
}

QStringList RepoFileIndex::named(const QString& name) const
{
    if (!m_indexed) {
        index();
    }

    QStringList result;
    for (const QString& path : m_paths) {
        if (QFileInfo(path).fileName() == name) {
            result.append(path);
        }
    }
    return result;
}

QStringList RepoFileIndex::all() const
{
    if (!m_indexed) {
        index();
    }
    return m_paths;
}

} // namespace graph_lang
