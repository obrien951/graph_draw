#pragma once
#include "rust_file_items.h"
#include "toml_document.h"
#include <QString>
#include <QStringList>
#include <QMap>
#include <QSet>

// RustAnalyzer: Discovers crates from Cargo.toml, derives depth-1 nested modules
// from the filesystem, scans items, and resolves edges.
// Crate label is package.name — the same name that appears in use statements and
// other crates.
class RustAnalyzer {
public:
    RustAnalyzer() = default;
    ~RustAnalyzer();

    // Analyze a repository and return discovered crates with their items.
    // The index should contain paths to Cargo.toml files.
    QMap<QString, CrateInfo> analyze(const QStringList& cargoTomlPaths);

    // Analyze a single Cargo.toml file.
    CrateInfo analyzeCargoToml(const QString& cargoTomlPath);

    // Get the last error message.
    QString lastError() const { return m_lastError; }

private:
    struct CrateInfo {
        QString name;
        QString version;
        QStringList sourceFiles;
        QMap<QString, RustFileItems> fileItems;  // relative path -> items
        QStringList crates;  // other crates this crate depends on
        QStringList modules;  // depth-1 nested modules derived from filesystem
    };

    QString m_lastError;

    // Parse Cargo.toml and extract crate info.
    CrateInfo parseCargoToml(const QString& path);

    // Derive depth-1 nested modules from the filesystem.
    QStringList deriveModules(const QString& crateDir, const QString& crateName);

    // Scan a single Rust file and return its items.
    RustFileItems scanFile(const QString& filePath, const QString& blankedText);

    // Resolve crate dependencies from Cargo.toml.
    QStringList resolveDependencies(const QString& cargoTomlPath);
};
