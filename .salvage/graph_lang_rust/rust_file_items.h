#ifndef GRAPH_LANG_RUST_RUST_FILE_ITEMS_H
#define GRAPH_LANG_RUST_RUST_FILE_ITEMS_H

#include <QObject>
#include <QString>
#include <QStringList>

namespace graph_lang_rust {

/**
 * @brief Per-file scan result: types, impl blocks, free functions,
 * use paths, and the file's inner //! doc.
 *
 * Produced by a single brace-depth scanner rather than global regex matches,
 * which is what keeps impls inside their parent types intact.
 */
class RustFileItems : public QObject {
    Q_OBJECT

public:
    explicit RustFileItems(QObject *parent = nullptr);
    ~RustFileItems() override = default;

    // The type name found in the item (struct, enum, impl, etc.)
    QString typeName;

    // The full impl block or function body
    QString implBlock;

    // The inner //! doc comment
    QString docComment;

    // Use paths found in the file
    QStringList usePaths;

    // Module path (e.g., "crate::module::type")
    QString modulePath;
};

} // namespace graph_lang_rust

#endif // GRAPH_LANG_RUST_RUST_FILE_ITEMS_H
