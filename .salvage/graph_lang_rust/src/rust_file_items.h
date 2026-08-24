#pragma once
#include <QString>
#include <QStringList>
#include <QMap>
#include <QSet>

// RustFileItems: per-file scan result containing types, impl blocks, free functions,
// use paths, and the file's inner //! doc. Produced by a single brace-depth scanner
// rather than global regex matches, which keeps impls inside macros from leaking.
class RustFileItems {
public:
    RustFileItems() = default;
    ~RustFileItems();

    // Set the file path this items belongs to.
    void setFilePath(const QString& path);

    // Types found in the file (structs, enums, unions, traits).
    QStringList types() const { return m_types; }

    // Impl blocks found in the file.
    QStringList implBlocks() const { return m_implBlocks; }

    // Free functions found in the file.
    QStringList freeFunctions() const { return m_freeFunctions; }

    // Use paths found in the file.
    QStringList usePaths() const { return m_usePaths; }

    // The file's inner //! doc comment.
    QString innerDoc() const { return m_innerDoc; }

    // All items combined.
    QStringList allItems() const;

    // Set a specific item list.
    void setTypes(const QStringList& types) { m_types = types; }
    void setImplBlocks(const QStringList& implBlocks) { m_implBlocks = implBlocks; }
    void setFreeFunctions(const QStringList& freeFunctions) { m_freeFunctions = freeFunctions; }
    void setUsePaths(const QStringList& usePaths) { m_usePaths = usePaths; }
    void setInnerDoc(const QString& doc) { m_innerDoc = doc; }

private:
    QString m_path;
    QStringList m_types;
    QStringList m_implBlocks;
    QStringList m_freeFunctions;
    QStringList m_usePaths;
    QString m_innerDoc;
};
