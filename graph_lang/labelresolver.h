#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace graph_lang {

// Two-pass unique-label picker.
//
// Node labels word-wrap into a 140x65 box with no eliding and "::" is not a
// wrap point, so crate-qualifying every Rust item renders as mush; but leaving
// names bare lets Error / Config / AppState collide across crates and the
// builder silently drops the second. observe() every candidate list first,
// then resolve() each item: it returns the first candidate no other item
// claims, the longest candidate when every one is contested, or the bare name
// when the list is empty.
class LabelResolver {
public:
    struct Item {
        QString     name;        // Bare name, used when candidates is empty.
        QStringList candidates;  // Labels this item could take.
    };

    LabelResolver() = default;

    void observe(const QVector<Item>& items);
    // The first candidate no other observed item claims; the longest when
    // every one is contested.
    QString resolve(const Item& item) const;

private:
    QHash<QString, int> m_claimCount;
};

}
