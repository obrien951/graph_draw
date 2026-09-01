#include "graphmerger.h"

#include <QHash>
#include <QList>
#include <QRectF>
#include <QString>

#include <qgraphicsitem>

#include "graphedge.h"
#include "graphnode.h"
#include "graphscene.h"

namespace {

struct Key {
    NodeType kind = NodeType::Module;
    QString label;

    bool operator==(const Key& other) const
    {
        return kind == other.kind && label == other.label;
    }
};

// Qt6's QHash finds its hash function via qHash(const Key&, size_t) through
// ADL rather than a functor template parameter (QHash only takes Key and T).
size_t qHash(const Key& key, size_t seed = 0)
{
    // Qualified so this doesn't recursively resolve to itself: an unqualified
    // call here would stop at this very overload during name lookup instead
    // of reaching Qt's global qHash(QString) / qHash(int) overloads.
    return ::qHash(key.label, seed) ^ ::qHash(static_cast<int>(key.kind), seed);
}

Key makeKey(NodeType kind, const QString& label)
{
    return Key{kind, label};
}

bool hasEdge(const GraphScene& scene,
             const GraphNode* source,
             const GraphNode* target,
             const QString& label)
{
    const QList<QGraphicsItem*> items = scene.items();
    for (QGraphicsItem* item : items) {
        const GraphEdge* edge = qgraphicsitem_cast<const GraphEdge*>(item);
        if (!edge) {
            continue;
        }
        if (edge->source() == source && edge->target() == target
            && edge->label() == label) {
            return true;
        }
    }
    return false;
}

constexpr double kXSpacing = 180.0;
constexpr double kYSpacing = 130.0;
constexpr double kPerRow = 12.0;

} // namespace

GraphMerger::Stats GraphMerger::merge(GraphScene* curated, const GraphScene* generated)
{
    Stats stats;

    if (!curated || !generated) {
        return stats;
    }

    const QList<GraphNode*> originalCurated = curated->nodes();
    QHash<Key, QList<GraphNode*>> curatedByKey;
    for (GraphNode* node : originalCurated) {
        if (!node) {
            continue;
        }
        curatedByKey[makeKey(node->kind(), node->label())].append(node);
    }

    QHash<Key, bool> onDisk;
    for (const GraphNode* node : generated->nodes()) {
        if (node) {
            onDisk.insert(makeKey(node->kind(), node->label()), true);
        }
    }

    QRectF curatedBox;
    bool hasCuratedBox = false;
    for (const GraphNode* node : originalCurated) {
        if (!node) {
            continue;
        }
        const QRectF rect = node->mapRectToScene(node->boundingRect());
        if (!hasCuratedBox) {
            curatedBox = rect;
            hasCuratedBox = true;
        } else {
            curatedBox = curatedBox.united(rect);
        }
    }

    const double baseY = hasCuratedBox ? curatedBox.bottom() + 1.0 : 0.0;
    int column = 0;

    QHash<const GraphNode*, GraphNode*> merged;

    for (const GraphNode* genNode : generated->nodes()) {
        if (!genNode) {
            continue;
        }

        const Key key = makeKey(genNode->kind(), genNode->label());
        const auto it = curatedByKey.constFind(key);

        if (it != curatedByKey.constEnd()) {
            for (GraphNode* kept : it.value()) {
                if (kept) {
                    kept->setImplemented(true);
                    if (kept->comment().isEmpty() && !genNode->comment().isEmpty()) {
                        kept->setComment(genNode->comment());
                    }
                }
            }
            merged.insert(genNode, it.value().first());
        } else {
            GraphNode* added = new GraphNode(genNode->kind(), genNode->label());
            if (!genNode->comment().isEmpty()) {
                added->setComment(genNode->comment());
            }
            added->setImplemented(true);

            const double x = (column % static_cast<int>(kPerRow)) * kXSpacing;
            const double y = baseY + (column / static_cast<int>(kPerRow)) * kYSpacing;
            added->setPos(x, y);
            ++column;

            curated->addItem(added);
            merged.insert(genNode, added);
            ++stats.added;
        }
    }

    const QList<QGraphicsItem*> genItems = generated->items();
    for (QGraphicsItem* item : genItems) {
        const GraphEdge* genEdge = qgraphicsitem_cast<const GraphEdge*>(item);
        if (!genEdge) {
            continue;
        }

        const GraphNode* genSource = genEdge->source();
        const GraphNode* genTarget = genEdge->target();
        if (!genSource || !genTarget) {
            continue;
        }

        auto sourceIt = merged.constFind(genSource);
        auto targetIt = merged.constFind(genTarget);
        if (sourceIt == merged.constEnd() || targetIt == merged.constEnd()) {
            continue;
        }

        GraphNode* source = sourceIt.value();
        GraphNode* target = targetIt.value();
        const QString label = genEdge->label();

        if (!hasEdge(*curated, source, target, label)) {
            curated->addItem(new GraphEdge(source, target, label));
        }
    }

    for (GraphNode* node : originalCurated) {
        if (!node) {
            continue;
        }

        const Key key = makeKey(node->kind(), node->label());
        if (onDisk.contains(key)) {
            ++stats.kept;
        } else {
            node->setImplemented(false);
            ++stats.stale;
        }
    }

    return stats;
}
