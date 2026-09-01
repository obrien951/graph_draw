#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

class TomlDocument {
public:
    struct Entry {
        QString path;
        QString rawValue;
    };

    static TomlDocument parse(const QString& text);

    void parseText(const QString& text);
    QStringList keys(const QString& prefix = QString()) const;
    bool contains(const QString& key) const;
    QString value(const QString& key) const;
    QStringList stringList(const QString& key) const;
    int tableArrayCount(const QString& key) const;

private:
    static QString unquote(const QString& raw);

    QVector<Entry> m_entries;
    QHash<QString, int> m_arrayTables;
};
