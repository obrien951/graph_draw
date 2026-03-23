#include "graphnode.h"
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QGraphicsDropShadowEffect>

GraphNode::GraphNode(NodeType kind, const QString& label, QGraphicsItem* parent)
    : QGraphicsObject(parent)
    , m_kind(kind)
    , m_label(label)
    , m_rect(-kW / 2.0, -kH / 2.0, kW, kH)
{
    setFlag(QGraphicsItem::ItemIsMovable);
    setFlag(QGraphicsItem::ItemIsSelectable);
    // Required so itemChange receives ItemScenePositionHasChanged,
    // which we use to push geometry updates to connected edges.
    setFlag(QGraphicsItem::ItemSendsScenePositionChanges);
    setZValue(1);   // nodes sit above edges (edges are z=0)

    auto* shadow = new QGraphicsDropShadowEffect;
    shadow->setBlurRadius(10);
    shadow->setOffset(3, 3);
    shadow->setColor(QColor(0, 0, 0, 70));
    setGraphicsEffect(shadow);
}

GraphNode::~GraphNode()
{
    // If this node is destroyed before its edges (e.g. via scene->clear()),
    // null out the raw pointers inside those edges so they don't dangle.
    for (const QPointer<GraphEdge>& edgePtr : qAsConst(m_edges)) {
        if (GraphEdge* edge = edgePtr.data()) {
            if (edge->source() == this) edge->clearSource();
            if (edge->target() == this) edge->clearTarget();
        }
    }
}

void GraphNode::setLabel(const QString& label)
{
    m_label = label;
    update();
}

void GraphNode::setComment(const QString& comment)
{
    m_comment = comment;
    // Surface the comment as a tooltip so it is visible on hover without
    // requiring any layout changes to the node itself.
    setToolTip(comment);
    update();
}

void GraphNode::addEdge(GraphEdge* edge)
{
    m_edges.append(QPointer<GraphEdge>(edge));
}

void GraphNode::removeEdge(GraphEdge* edge)
{
    // Remove all entries that either match the pointer or have gone null
    m_edges.erase(
        std::remove_if(m_edges.begin(), m_edges.end(),
                       [edge](const QPointer<GraphEdge>& p) {
                           return p.isNull() || p.data() == edge;
                       }),
        m_edges.end());
}

QPointF GraphNode::borderPoint(const QPointF& towardScene) const
{
    const QPointF center = mapToScene(QPointF(0, 0));
    if (center == towardScene)
        return center;

    // Map the local shape rect to scene coordinates.
    // Since nodes are not rotated, this is equivalent to a simple translation.
    const QRectF sr = mapRectToScene(m_rect);
    const QLineF  ray(center, towardScene);

    const QLineF sides[4] = {
        {sr.topLeft(),     sr.topRight()},
        {sr.topRight(),    sr.bottomRight()},
        {sr.bottomRight(), sr.bottomLeft()},
        {sr.bottomLeft(),  sr.topLeft()},
    };

    QPointF hit;
    for (const QLineF& side : sides) {
        if (ray.intersects(side, &hit) == QLineF::BoundedIntersection)
            return hit;
    }
    return center;
}

QRectF GraphNode::boundingRect() const
{
    // Small margin beyond m_rect to accommodate the selection highlight pen
    return m_rect.adjusted(-3, -3, 3, 3);
}

QPainterPath GraphNode::shape() const
{
    QPainterPath path;
    switch (m_kind) {
    case NodeType::Class:
        path.addRect(m_rect);
        break;
    case NodeType::Module:
        path.addRoundedRect(m_rect, 10, 10);
        break;
    case NodeType::Function:
        path.addEllipse(m_rect);
        break;
    }
    return path;
}

void GraphNode::paint(QPainter* painter,
                      const QStyleOptionGraphicsItem* option,
                      QWidget*)
{
    painter->setRenderHint(QPainter::Antialiasing);

    // ── Colour scheme per node type ─────────────────────────────────────────
    QColor fillColor, headerColor, borderColor;
    QString stereotype;

    switch (m_kind) {
    case NodeType::Class:
        fillColor   = QColor(74, 144, 196);
        headerColor = QColor(50, 105, 150);
        borderColor = QColor(35,  80, 120);
        stereotype  = QStringLiteral("«class»");
        break;
    case NodeType::Module:
        fillColor   = QColor(80, 170, 100);
        headerColor = QColor(55, 125,  70);
        borderColor = QColor(35,  95,  50);
        stereotype  = QStringLiteral("«module»");
        break;
    case NodeType::Function:
        fillColor   = QColor(212, 132, 74);
        headerColor = QColor(165,  95, 45);
        borderColor = QColor(130,  70, 25);
        stereotype  = QStringLiteral("«function»");
        break;
    }

    if (option->state & QStyle::State_Selected) {
        fillColor   = fillColor.lighter(130);
        borderColor = QColor(255, 220, 0);
    }

    // ── Draw body ────────────────────────────────────────────────────────────
    QPen borderPen(borderColor, option->state & QStyle::State_Selected ? 2.5 : 1.5);
    painter->setPen(borderPen);
    painter->setBrush(fillColor);

    switch (m_kind) {
    case NodeType::Class:    painter->drawRect(m_rect);                    break;
    case NodeType::Module:   painter->drawRoundedRect(m_rect, 10, 10);    break;
    case NodeType::Function: painter->drawEllipse(m_rect);                 break;
    }

    // ── Header strip (top ~25 % of the node) ────────────────────────────────
    const qreal headerH = m_rect.height() * 0.30;
    const QRectF headerRect(m_rect.left(), m_rect.top(), m_rect.width(), headerH);

    painter->setPen(Qt::NoPen);
    painter->setBrush(headerColor);

    // Clip the header fill to the node's own shape so rounded/ellipse corners
    // are respected.
    painter->save();
    painter->setClipPath(shape());
    painter->drawRect(headerRect);
    painter->restore();

    // Divider line between header and body
    painter->setPen(QPen(borderColor, 0.8));
    painter->drawLine(m_rect.left(),  m_rect.top() + headerH,
                      m_rect.right(), m_rect.top() + headerH);

    // ── Stereotype text (header) ─────────────────────────────────────────────
    QFont stereoFont = painter->font();
    stereoFont.setPointSizeF(7.0);
    stereoFont.setItalic(true);
    stereoFont.setBold(false);
    painter->setFont(stereoFont);
    painter->setPen(QColor(255, 255, 255, 200));
    painter->drawText(headerRect, Qt::AlignCenter, stereotype);

    // ── Label text (body) ────────────────────────────────────────────────────
    const QRectF bodyRect(m_rect.left(),  m_rect.top() + headerH,
                          m_rect.width(), m_rect.height() - headerH);
    QFont labelFont = painter->font();
    labelFont.setPointSizeF(9.0);
    labelFont.setItalic(false);
    labelFont.setBold(true);
    painter->setFont(labelFont);
    painter->setPen(Qt::white);
    painter->drawText(bodyRect, Qt::AlignCenter | Qt::TextWordWrap, m_label);
}

QVariant GraphNode::itemChange(GraphicsItemChange change, const QVariant& value)
{
    if (change == ItemScenePositionHasChanged) {
        for (const QPointer<GraphEdge>& edgePtr : qAsConst(m_edges)) {
            if (GraphEdge* edge = edgePtr.data())
                edge->updateGeometry();
        }
    }
    return QGraphicsObject::itemChange(change, value);
}
