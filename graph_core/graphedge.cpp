#include "graphedge.h"
#include "graphnode.h"
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QStyleOptionGraphicsItem>
#include <QFontMetricsF>

GraphEdge::GraphEdge(GraphNode* source, GraphNode* target,
                     const QString& label, QGraphicsItem* parent)
    : QGraphicsObject(parent)
    , m_source(source)
    , m_target(target)
    , m_label(label)
{
    setFlag(QGraphicsItem::ItemIsSelectable);
    setZValue(0);   // edges sit below nodes (nodes are z=1)
    setPos(0, 0);   // all geometry computed in scene coordinates

    if (m_source) m_source->addEdge(this);
    if (m_target && m_target != m_source) m_target->addEdge(this);

    recomputeGeometry();
}

GraphEdge::~GraphEdge()
{
    // Unregister from both nodes before the object is gone.
    // Guards against m_source / m_target already being null if a node was
    // deleted and called clearSource() / clearTarget() on us first.
    if (m_source) m_source->removeEdge(this);
    if (m_target && m_target != m_source) m_target->removeEdge(this);
}

void GraphEdge::setLabel(const QString& label)
{
    m_label = label;
    recomputeGeometry();
    update();
}

void GraphEdge::clearSource() { m_source = nullptr; }
void GraphEdge::clearTarget() { m_target = nullptr; }

void GraphEdge::updateGeometry()
{
    recomputeGeometry();
    update();
}

void GraphEdge::recomputeGeometry()
{
    prepareGeometryChange();

    if (!m_source || !m_target) {
        m_line = QLineF();
        m_arrowHead.clear();
        m_cachedBoundingRect = QRectF();
        return;
    }

    const QPointF srcCenter = m_source->mapToScene(QPointF(0, 0));
    const QPointF dstCenter = m_target->mapToScene(QPointF(0, 0));

    const QPointF srcPt = m_source->borderPoint(dstCenter);
    const QPointF dstPt = m_target->borderPoint(srcCenter);

    m_line = QLineF(srcPt, dstPt);

    // Arrowhead — filled triangle at the target end
    if (m_line.length() > 1.0) {
        const double angle = std::atan2(-m_line.dy(), m_line.dx());
        const QPointF tip  = m_line.p2();
        const QPointF w1   = tip + QPointF(-kArrowSize * std::cos(angle - kArrowSpread),
                                            kArrowSize * std::sin(angle - kArrowSpread));
        const QPointF w2   = tip + QPointF(-kArrowSize * std::cos(angle + kArrowSpread),
                                            kArrowSize * std::sin(angle + kArrowSpread));
        m_arrowHead = QPolygonF({tip, w1, w2});
    } else {
        m_arrowHead.clear();
    }

    // Bounding rect covers line + arrowhead + optional label
    QRectF r = QRectF(srcPt, dstPt).normalized().adjusted(-2, -2, 2, 2);
    if (!m_arrowHead.isEmpty())
        r = r.united(m_arrowHead.boundingRect().adjusted(-2, -2, 2, 2));

    if (!m_label.isEmpty()) {
        const QPointF mid = (srcPt + dstPt) / 2.0;
        r = r.united(QRectF(mid.x() - 60, mid.y() - 14, 120, 28));
    }

    m_cachedBoundingRect = r;
}

QRectF GraphEdge::boundingRect() const
{
    return m_cachedBoundingRect;
}

QPainterPath GraphEdge::shape() const
{
    // Widen the hit area so the edge is easy to click
    QPainterPath linePath;
    linePath.moveTo(m_line.p1());
    linePath.lineTo(m_line.p2());

    QPainterPathStroker stroker;
    stroker.setWidth(10.0);
    QPainterPath result = stroker.createStroke(linePath);

    if (!m_arrowHead.isEmpty()) {
        QPainterPath arrowPath;
        arrowPath.addPolygon(m_arrowHead);
        result = result.united(arrowPath);
    }
    return result;
}

void GraphEdge::paint(QPainter* painter,
                      const QStyleOptionGraphicsItem* option,
                      QWidget*)
{
    if (!m_source || !m_target || m_line.length() < 1.0)
        return;

    painter->setRenderHint(QPainter::Antialiasing);

    const bool selected = option->state & QStyle::State_Selected;
    const QColor edgeColor = selected ? QColor(255, 200, 0) : QColor(70, 70, 70);

    // Line
    QPen pen(edgeColor, selected ? 2.5 : 1.8, Qt::SolidLine, Qt::RoundCap);
    painter->setPen(pen);
    painter->drawLine(m_line);

    // Filled arrowhead
    if (!m_arrowHead.isEmpty()) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(edgeColor);
        painter->drawPolygon(m_arrowHead);
    }

    // Label with background pill for readability
    if (!m_label.isEmpty()) {
        const QPointF mid = (m_line.p1() + m_line.p2()) / 2.0;

        QFont font = painter->font();
        font.setPointSizeF(8.0);
        painter->setFont(font);

        const QFontMetricsF fm(font);
        const QRectF textRect = fm.boundingRect(m_label).adjusted(-5, -2, 5, 2)
                                  .translated(mid - fm.boundingRect(m_label).center());

        painter->setBrush(QColor(255, 255, 220, 220));
        painter->setPen(QPen(QColor(180, 180, 120), 0.5));
        painter->drawRoundedRect(textRect, 3, 3);

        painter->setPen(QColor(40, 40, 40));
        painter->drawText(textRect, Qt::AlignCenter, m_label);
    }
}
