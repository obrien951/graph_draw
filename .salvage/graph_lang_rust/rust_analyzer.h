#ifndef GRAPH_LANG_RUST_RUST_ANALYZER_H
#define GRAPH_LANG_RUST_RUST_ANALYZER_H

#include <QObject>
#include <QDirIterator>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>
#include <QVariant>
#include <QVariantMap>
#include <QVariantList>

#include "repo_file_index.h"
#include "toml_document.h"
#include "rust_file_items.h"
#include "label_resolver.h"
#include "language_analyzer.h"
#include "ir/repo.h"

namespace graph_lang_rust {

class RustAnalyzer : public LanguageAnalyzer {
    Q_OBJECT

public:
    explicit RustAnalyzer(QObject *parent = nullptr);
    ~RustAnalyzer() override;

    // NEW (B6-B8). Discovers crates from Cargo.toml, derives depth-1 nested modules
    // from the filesystem, scans items, and resolves edges.
    // Crate label is package.name — the same name that appears in use statements
    // and other crates' [dependencies], which makes edge resolution trivial.
    bool detect(const RepoFileIndex &index) const override;
    ir::Repo analyze(const RepoFileIndex &index) const override;

private:
    // NEW (B5). Read-only Cargo.toml reader flattened to dotted paths.
    // Insertion-ordered storage so keys() follows file order.
    TomlDocument parseCargoToml(const QString &path) const;

    // NEW (B7). Per-file scan result: types, impl blocks, free functions,
    // use paths, and the file's inner //! doc.
    RustFileItems scanRustFile(const QString &path) const;

    // NEW (B8). Two-pass shortest-unique-label picker.
    // Node labels word-wrap into a 140x65 box with no eliding and '::' is not
    // a wrap point, so crate-qualifying every Rust item renders as mush;
    // but leaving names bare lets Erroneous::Foo become Foo, which is what
    // the graph wants.
    QString resolveLabel(const QString &crateName, const QString &itemName) const;

    // Derives depth-1 nested modules from the filesystem.
    // [lib] never creates a second module; [[bin]] does only when its name
    // differs from the package name.
    QStringList deriveModules(const QString &crateDir, const TomlDocument &toml) const;

    // Scans items and resolves edges.
    void resolveEdges(const QString &crateDir, const TomlDocument &toml,
                      const QStringList &modules, ir::Repo &repo) const;
};

} // namespace graph_lang_rust

#endif // GRAPH_LANG_RUST_RUST_ANALYZER_H
