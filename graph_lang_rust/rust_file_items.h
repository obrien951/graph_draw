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
    };

    static RustFileItems empty();

    QVector<Type> types;
    QVector<Item> functions;
    QStringList usePaths;
    QString innerDoc;
};

RustFileItems scanRustFile(const QString& source);
