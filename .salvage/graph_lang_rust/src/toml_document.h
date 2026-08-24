#pragma once
#include <QString>
#include <QStringList>
#include <QMap>
#include <QVariant>

// TomlDocument: read-only Cargo.toml reader flattened to dotted paths.
// Keys are ordered as they appear in the file (insertion-ordered storage).
// Returns dotted paths like "package.name", "bin[1].name", "dependencies.sqlx.version".
class TomlDocument {
public:
    TomlDocument() = default;
    ~TomlDocument();

    // Parse a Cargo.toml file from a file path. Returns true on success.
    bool parse(const QString& filePath);

    // Returns the raw file content as a QString.
    QString rawText() const { return m_rawText; }

    // Returns all keys in file order (insertion-ordered).
    QStringList keys() const { return m_keys; }

    // Returns the value at a dotted path, or empty QVariant if not found.
    QVariant get(const QString& path) const;

    // Returns true if the path exists.
    bool has(const QString& path) const { return m_paths.contains(path); }

    // Returns the number of top-level keys.
    int size() const { return m_paths.size(); }

private:
    QString m_rawText;
    QStringList m_keys;          // insertion-ordered list of all keys
    QMap<QString, QVariant> m_paths;  // dotted path -> value
    QMap<QString, QVariant> m_nested; // nested access for dotted paths
};
