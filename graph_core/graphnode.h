#pragma once
#include "graphedge.h"        // full definition needed to instantiate QPointer<GraphEdge>
#include <QGraphicsObject>
#include <QList>
#include <QPointer>
#include <QString>

enum class NodeType { Class, Module, Function };

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
    NodeType kind()  const { return m_kind; }
    QString  label() const { return m_label; }
    void     setLabel(const QString& label);

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

private:
    NodeType                   m_kind;
    QString                    m_label;
    QList<QPointer<GraphEdge>> m_edges;   // non-owning, self-nulling observers
    QRectF                     m_rect;    // local shape rect, centered at origin

    static constexpr qreal kW = 140.0;
    static constexpr qreal kH = 65.0;
};
