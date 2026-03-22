#pragma once
#include <QGraphicsObject>
#include <QLineF>
#include <QPolygonF>
#include <QPointer>
#include <QString>
#include <cmath>

class GraphNode;

class GraphEdge : public QGraphicsObject {
    Q_OBJECT
public:
    enum { Type = UserType + 2 };
    int type() const override { return Type; }

    explicit GraphEdge(GraphNode* source, GraphNode* target,
                       const QString& label = QString(),
                       QGraphicsItem* parent = nullptr);
    ~GraphEdge() override;

    // Raw pointers here: GraphEdge is always deleted before its nodes by the
    // scene's deletion helpers, so these are never dangling in practice.
    GraphNode* source() const { return m_source; }
    GraphNode* target() const { return m_target; }

    QString label()  const { return m_label; }
    void    setLabel(const QString& label);

    // Called by GraphNode::~GraphNode() to nullify the raw pointer before the
    // node is destroyed, preventing a dangling pointer in updateGeometry().
    void clearSource();
    void clearTarget();

    // Called by GraphNode::itemChange whenever either endpoint moves
    void updateGeometry();

    QRectF       boundingRect() const override;
    QPainterPath shape()        const override;
    void         paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override;

private:
    GraphNode* m_source;      // non-owning; scene ensures this outlives the edge
    GraphNode* m_target;      // non-owning; same contract
    QString    m_label;

    QLineF    m_line;                  // clipped line between node boundaries, scene coords
    QPolygonF m_arrowHead;             // precomputed filled triangle at target end
    QRectF    m_cachedBoundingRect;    // updated by recomputeGeometry()

    void recomputeGeometry();

    static constexpr qreal kArrowSize   = 12.0;
    static constexpr qreal kArrowSpread = M_PI / 6.0;   // 30 degrees
};
