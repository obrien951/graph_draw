#pragma once
//
// Shared scene-inspection helpers for the test suite.
//
// tests/test_repoanalyzer.cpp is the regression contract for the Part B
// refactor and must stay byte-identical, so it keeps its own `static` copies of
// nodeNamed/hasEdge/nodesOfKind. Those have INTERNAL linkage, and this header is
// not included there, so the `inline` (external linkage, vague) definitions
// below live only in other translation units and cannot clash with them at link
// time. No namespace is needed.
//
// Everything here is read-only: nothing mutates the scene.

#include "graphscene.h"
#include "graphnode.h"
#include "graphedge.h"

#include <QGraphicsItem>
#include <QList>
#include <QString>

// Every GraphNode in the scene whose kind() matches.
// Order follows GraphScene::nodes(), i.e. QGraphicsScene::items() stacking
// order, which for equal-Z items is reverse insertion order.
inline QList<GraphNode*> nodesOfKind(GraphScene& scene, NodeType kind)
{
    QList<GraphNode*> result;
    for (GraphNode* n : scene.nodes())
        if (n->kind() == kind) result.append(n);
    return result;
}

// The first node carrying this label, or nullptr if the scene has none.
inline GraphNode* nodeNamed(GraphScene& scene, const QString& label)
{
    for (GraphNode* n : scene.nodes())
        if (n->label() == label) return n;
    return nullptr;
}

// True if some edge runs from the node labelled `from` to the node labelled
// `to`. Direction matters: an edge A -> B means "A depends on B".
inline bool hasEdge(GraphScene& scene, const QString& from, const QString& to)
{
    for (QGraphicsItem* item : scene.items())
        if (auto* e = qgraphicsitem_cast<GraphEdge*>(item))
            if (e->source() && e->target() &&
                e->source()->label() == from && e->target()->label() == to)
                return true;
    return false;
}

// The label of the edge from `from` to `to`, or a null QString if no such edge
// exists. Note the distinction: a present-but-unlabelled edge yields an empty
// but NON-null QString, so isNull() means "no edge" and isEmpty() does not.
inline QString edgeLabel(GraphScene& scene, const QString& from, const QString& to)
{
    for (QGraphicsItem* item : scene.items())
        if (auto* e = qgraphicsitem_cast<GraphEdge*>(item))
            if (e->source() && e->target() &&
                e->source()->label() == from && e->target()->label() == to)
                return e->label();
    return QString();
}

// How many GraphEdges the scene currently holds.
inline int edgeCount(GraphScene& scene)
{
    int count = 0;
    for (QGraphicsItem* item : scene.items())
        if (qgraphicsitem_cast<GraphEdge*>(item)) ++count;
    return count;
}
