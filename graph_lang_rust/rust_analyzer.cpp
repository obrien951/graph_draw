#include "rust_analyzer.h"
#include "repofileindex.h"
#include "toml_document.h"
#include "ir.h"

#include <stdexcept>
#include <QSet>
#include <QDir>
#include <QFile>
#include <QRegularExpression>

QString RustAnalyzer::id() const
{
    return QStringLiteral("rust");
}

bool RustAnalyzer::detect(const RepoFileIndex& index) const
{
    return !index.named("Cargo.toml").isEmpty();
}

bool RustAnalyzer::analyze(const RepoFileIndex& index, ir::Repo* repo, QString* error) const
{
    // NEW (B6-B8). Crates from [workspace] members plus every Cargo.toml, deduped by directory and sorted by crate name.
    // [lib] never creates a second module; [[bin]] does only when its name differs from the package name.
    // Nested modules are depth-1 and derived from src/ paths, so no mod resolution is needed and the tree cannot disagree with itself.
    
    // Collect all Cargo.toml files
    const auto cargoFiles = index.named("Cargo.toml");
    
    // Deduplicate by directory and collect crates
    QSet<QString> seenDirectories;
    QStringList crateNames;
    QStringList crateDirectories;
    
    for (const QString& cargoPath : cargoFiles) {
        // Get directory of the Cargo.toml
        QDir dir = QDir(cargoPath).absolutePath();
        QString dirPath = dir.absolutePath();
        
        // Skip if already processed this directory
        if (seenDirectories.contains(dirPath)) {
            continue;
        }
        
        seenDirectories.insert(dirPath);
        
        // Read and parse the Cargo.toml
        QFile file(cargoPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (error) {
                *error = QString("Failed to open Cargo.toml at %1").arg(cargoPath);
            }
            return false;
        }
        
        QString cargoContent = file.readAll();
        file.close();
        
        if (cargoContent.isEmpty()) {
            if (error) {
                *error = QString("Failed to read Cargo.toml content at %1").arg(cargoPath);
            }
            return false;
        }
        
        TomlDocument doc = TomlDocument::parse(cargoContent);
        
        // Extract crate name from package.name
        QString crateName = doc.value("package.name");
        if (crateName.isEmpty()) {
            // If no package.name, skip this Cargo.toml
            continue;
        }
        
        // Add to crate list
        crateNames.append(crateName);
        crateDirectories.append(dirPath);
    }
    
    // Sort crate names and their corresponding directories together
    // This maintains the pairing between crate name and directory
    QStringList sortedCrateNames;
    QStringList sortedDirectories;
    
    // Create pairs of (crateName, directory) and sort them by crate name
    QVector<QPair<QString, QString>> cratePairs;
    for (int i = 0; i < crateNames.size(); ++i) {
        cratePairs.append(QPair<QString, QString>(crateNames[i], crateDirectories[i]));
    }
    
    std::sort(cratePairs.begin(), cratePairs.end(), 
              [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) {
                  return a.first < b.first;
              });
    
    for (const auto& pair : cratePairs) {
        sortedCrateNames.append(pair.first);
        sortedDirectories.append(pair.second);
    }
    
    // Create modules for each crate
    for (int i = 0; i < sortedCrateNames.size(); ++i) {
        const QString& crateName = sortedCrateNames[i];
        const QString& crateDir = sortedDirectories[i];
        
        // Create a module for the crate
        ir::Module module;
        module.label = crateName;
        module.description = QString("Rust crate: %1").arg(crateName);
        
        // Add the module to the repo using the proper method
        repo->addModule(module);
    }
    
    return true;
}