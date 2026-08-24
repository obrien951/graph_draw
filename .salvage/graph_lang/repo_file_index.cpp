#include "repo_file_index.h"
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

namespace graph_lang {

RepoFileIndex::RepoFileIndex(const QString& root, const QStringList& skipDirs)
    : root_(root), skippedDirs_(skipDirs) {
    if (root_.isEmpty()) {
        root_ = QDir::currentPath();
    }
    files_.clear();
    walkDir(root_, skipDirs_, files_);
}

void RepoFileIndex::walkDir(const QString& dir, const QStringList& skipDirs, QVector<QString>& files) {
    QDir directory(dir);
    if (!directory.exists()) {
        return;
    }

    // Collect all entries first
    QSet<QString> entries;
    for (const QFileInfo& info : directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        entries.insert(info.fileName());
    }

    // Separate files and directories
    QVector<QString> filesInDir;
    QVector<QString> dirsInDir;

    for (const QString& entry : entries) {
        QFileInfo info(dir + "/" + entry);
        if (info.isFile()) {
            filesInDir.append(info.absoluteFilePath());
        } else if (info.isDir()) {
            dirsInDir.append(entry);
        }
    }

    // Add files to the result
    for (const QString& file : filesInDir) {
        files.append(file);
    }

    // Recurse into directories that are NOT in the skip list
    for (const QString& dirEntry : dirsInDir) {
        if (!skipDirs_.contains(dirEntry)) {
            walkDir(dir + "/" + dirEntry, skipDirs, files);
        }
    }
}

QVector<QString> RepoFileIndex::withSuffix(const QStringList& suffixes, const QString& root) {
    RepoFileIndex index(root, QStringList());
    return index.filesWithSuffix(suffixes);
}

QVector<QString> RepoFileIndex::allFiles() const {
    return files_;
}

QVector<QString> RepoFileIndex::filesWithSuffix(const QStringList& suffixes) const {
    QVector<QString> result;
    for (const QString& file : files_) {
        QFileInfo info(file);
        QString lowerName = info.fileName().toLower();
        for (const QString& suffix : suffixes) {
            if (lowerName.endsWith(suffix.toLower())) {
                result.append(file);
                break;
            }
        }
    }
    return result;
}

} // namespace graph_lang
