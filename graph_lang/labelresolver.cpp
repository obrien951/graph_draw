#include "labelresolver.h"

#include <QSet>

namespace graph_lang {

void LabelResolver::observe(const QVector<Item>& items)
{
    for (const Item& item : items) {
        QSet<QString> claimed;
        for (const QString& candidate : item.candidates) {
            if (candidate.isEmpty())
                continue;
            if (claimed.contains(candidate))
                continue;
            claimed.insert(candidate);
            m_claimCount[candidate]++;
        }
    }
}

QString LabelResolver::resolve(const Item& item) const
{
    // "" is not a usable label; drop it so items that only carry empty
    // candidates fall back to their bare name.
    QStringList candidates = item.candidates;
    candidates.removeAll(QString());

    if (candidates.isEmpty())
        return item.name;

    for (const QString& candidate : candidates) {
        if (m_claimCount.value(candidate, 0) <= 1)
            return candidate;
    }

    QString longest = candidates.first();
    for (const QString& candidate : candidates) {
        if (candidate.length() > longest.length())
            longest = candidate;
    }
    return longest;
}

}
