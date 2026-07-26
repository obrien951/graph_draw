#pragma once
#include "graphedge.h"        // full definition needed to instantiate QPointer<GraphEdge>
#include <QGraphicsObject>
#include <QList>
#include <QPointer>
#include <QString>

enum class NodeType { Module, Class, Function };

// Readiness of a node relative to the things it depends on.
// Used by agents/tools to decide whether a node can be implemented against
// real code, or whether its dependencies must first be built or stubbed.
enum class DependencyStatus {
    NoDependencies,  // leaf node — nothing to depend on
    Ready,           // every dependency is already implemented
    Blocked          // one or more dependencies are not yet implemented
};

class GraphNode : public QGraphicsObject {
    Q_OBJECT
public:
    // Qt runtime type tag — enables qgraphicsitem_cast<GraphNode*>
    enum { Type = UserType + 1 };
    int type() const override { return Type; }

    explicit GraphNode(NodeType kind, const QString& label,
                       QGraphicsItem* parent = nullptr);
    ~GraphNode() override;

    // kind() returns the semantic category (Class / Module / Function).
    // Named differently from Qt's type() to avoid any confusion.
    NodeType kind()    const { return m_kind; }
    QString  label()   const { return m_label; }

    // The comment doubles as this node's implementation prompt/description —
    // the text used to implement the module, class, or function it stands for.
    QString  comment() const { return m_comment; }
    void setLabel(const QString& label);
    void setComment(const QString& comment);

    // Whether the code this node represents has actually been implemented.
    // Dependency tracking reads this flag to report, for any node, whether the
    // things it relies on already exist or still need to be built or stubbed.
    bool isImplemented() const { return m_implemented; }
    void setImplemented(bool implemented);

    // ── Dependency queries ──────────────────────────────────────────────────
    // Edge-direction convention: an edge A → B (arrow pointing at B) means
    // "A depends on B". So a node's dependencies are the targets of its
    // outgoing edges; its dependents are the sources of its incoming edges.
    // Self-loops and duplicate connections are ignored.
    QList<GraphNode*> dependencies() const;
    QList<GraphNode*> dependents()   const;

    // The subset of dependencies() that are not yet implemented — the pieces
    // that must be built (or stubbed with a NotImplementedError) before this
    // node can be implemented against real code.
    QList<GraphNode*> unimplementedDependencies() const;

    // Readiness of this node with respect to its dependencies.
    DependencyStatus dependencyStatus() const;

    // Non-owning edge registry.
    // QPointer<GraphEdge> nulls itself automatically if an edge is deleted
    // outside the normal scene helpers, preventing silent use-after-free.
    void addEdge(GraphEdge* edge);
    void removeEdge(GraphEdge* edge);
    QList<QPointer<GraphEdge>> edges() const { return m_edges; }

    // Returns the point on this node's boundary in the direction of towardScene.
    // Used by GraphEdge to clip its arrow endpoints to the node silhouette.
    QPointF borderPoint(const QPointF& towardScene) const;

    QRectF       boundingRect() const override;
    QPainterPath shape()        const override;
    void         paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    NodeType                   m_kind;
    QString                    m_label;
    QString                    m_comment;
    bool                       m_implemented = false;
    QList<QPointer<GraphEdge>> m_edges;   // non-owning, self-nulling observers
    QRectF                     m_rect;    // local shape rect, centered at origin

    static constexpr qreal kW = 140.0;
    static constexpr qreal kH = 65.0;
};
