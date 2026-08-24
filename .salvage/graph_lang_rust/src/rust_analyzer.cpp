#include "rust_analyzer.h"
#include "scan_rust_file.h"
#include "toml_document.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QDebug>

RustAnalyzer::~RustAnalyzer() = default;

QMap<QString, CrateInfo> RustAnalyzer::analyze(const QStringList& cargoTomlPaths)
{
    QMap<QString, CrateInfo> result;

    for (const QString& path : cargoTomlPaths) {
        CrateInfo crate = analyzeCargoToml(path);
        if (!crate.name.isEmpty()) {
            result.insert(crate.name, crate);
        }
    }

    return result;
}

CrateInfo RustAnalyzer::analyzeCargoToml(const QString& cargoTomlPath)
{
    CrateInfo crate;

    // Parse Cargo.toml
    TomlDocument toml;
    if (!toml.parse(cargoTomlPath)) {
        m_lastError = "Failed to parse Cargo.toml: " + cargoTomlPath;
        return crate;
    }

    // Extract package.name
    QVariant nameVal = toml.get("package.name");
    if (nameVal.isValid()) {
        crate.name = nameVal.toString();
    }

    // Extract package.version
    QVariant versionVal = toml.get("package.version");
    if (versionVal.isValid()) {
        crate.version = versionVal.toString();
    }

    // Extract source files (lib.rs, src/*.rs, bin/*.rs, tests/*.rs)
    QString crateDir = QFileInfo(cargoTomlPath).absolutePath();
    QDir dir(crateDir);

    // lib.rs
    QString libPath = dir.absoluteFilePath("lib.rs");
    if (QFile::exists(libPath)) {
        crate.sourceFiles.append(libPath);
    }

    // src/*.rs
    QStringList srcFiles = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString& file : srcFiles) {
        if (file.endsWith(".rs")) {
            crate.sourceFiles.append(dir.absoluteFilePath(file));
        }
    }

    // bin/*.rs
    QStringList binFiles = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString& file : binFiles) {
        if (file.endsWith(".rs")) {
            crate.sourceFiles.append(dir.absoluteFilePath(file));
        }
    }

    // tests/*.rs
    QStringList testFiles = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString& file : testFiles) {
        if (file.endsWith(".rs")) {
            crate.sourceFiles.append(dir.absoluteFilePath(file));
        }
    }

    // Resolve dependencies
    crate.crates = resolveDependencies(cargoTomlPath);

    // Derive depth-1 nested modules
    crate.modules = deriveModules(crateDir, crate.name);

    // Scan each source file
    for (const QString& filePath : crate.sourceFiles) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        QString text = QString::fromUtf8(file.readAll());
        file.close();

        // Blank the text
        QString blanked = blank(text);

        // Scan the file
        RustFileItems items = scanFile(filePath, blanked);
        items.setFilePath(filePath);
        crate.fileItems.insert(QFileInfo(filePath).fileName(), items);
    }

    return crate;
}

QStringList RustAnalyzer::resolveDependencies(const QString& cargoTomlPath)
{
    TomlDocument toml;
    if (!toml.parse(cargoTomlPath)) {
        return QStringList();
    }

    QStringList deps;
    for (const QString& key : toml.keys()) {
        if (key.startsWith("dependencies.")) {
            deps.append(key);
        }
    }
    return deps;
}

QStringList RustAnalyzer::deriveModules(const QString& crateDir, const QString& crateName)
{
    QStringList modules;
    QDir dir(crateDir);

    // Look for src/*.rs files and derive module names from directory structure
    QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        QString fullPath = dir.absoluteFilePath(entry);
        QDir subdir(fullPath);
        QStringList subEntries = subdir.entryList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QString& file : subEntries) {
            if (file.endsWith(".rs")) {
                // Derive module name from directory name
                modules.append(entry);
            }
        }
    }

    return modules;
}

RustFileItems RustAnalyzer::scanFile(const QString& filePath, const QString& blankedText)
{
    return scanRustFile(filePath, blankedText);
}
