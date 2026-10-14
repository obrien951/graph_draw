#pragma once

#include <QVector>
#include <QString>
#include <QStringList>

class RustFileItems {
public:
    struct Item {
        QString label;
        int line = 0;
    };

    struct Type {
        QString label;
        int line = 0;
        QVector<Item> methods;
        // Trait names from "impl Trait for Type" blocks that target this
        // type (deduped). Empty for a type with only inherent impls, or
        // none at all.
        QStringList implementsTraits;
    };

    static RustFileItems empty();

    QVector<Type> types;
    QVector<Item> functions;
    QStringList usePaths;
    QString innerDoc;
};

RustFileItems scanRustFile(const QString& source);
