#ifndef GRAPH_LANG_RUST_REPO_FILE_INDEX_H
#define GRAPH_LANG_RUST_REPO_FILE_INDEX_H

#include <QObject>
#include <QDirEntryInfo>
#include <QString>
#include <QStringList>

namespace graph_lang_rust {

/**
 * @brief One filesystem walk shared by every analyzer, with build and VCS
 * noise removed. Must NOT descend into skipped directories — today's
 * QDirIterator descends and filters afterwards, which is merely wasteful
 * for C++ projects with thousands of build artifacts.
 */
class RepoFileIndex : public QObject {
    Q_OBJECT

public:
    explicit RepoFileIndex(QObject *parent = nullptr);
    ~RepoFileIndex() override = default;

    // Add a file entry
    void addFile(const QString &path);

    // Add a directory entry
    void addDir(const QString &path);

    // Get all files
    QStringList files() const;

    // Get all directories
    QStringList dirs() const;

    // Check if a path is a build artifact
    bool isBuildArtifact(const QString &path) const;

    // Check if a path is a VCS artifact
    bool isVcsArtifact(const QString &path) const;

private:
    QStringList m_files;
    QStringList m_dirs;
};

} // namespace graph_lang_rust

#endif // GRAPH_LANG_RUST_REPO_FILE_INDEX_H
