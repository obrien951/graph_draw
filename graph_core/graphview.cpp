#include "graphview.h"
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <QPainter>

GraphView::GraphView(QWidget* parent)
    : QGraphicsView(parent)
{
    setRenderHint(QPainter::Antialiasing);
    setRenderHint(QPainter::SmoothPixmapTransform);
    setTransformationAnchor(AnchorUnderMouse);
    setResizeAnchor(AnchorViewCenter);
    setDragMode(NoDrag);   // we manage panning ourselves
    setViewportUpdateMode(SmartViewportUpdate);
    setBackgroundBrush(QColor(248, 248, 252));
}

// ── Zoom ──────────────────────────────────────────────────────────────────────

void GraphView::wheelEvent(QWheelEvent* event)
{
    const double factor = (event->angleDelta().y() > 0) ? kZoomStep : 1.0 / kZoomStep;

    // Clamp the accumulated scale
    const double current = transform().m11();   // horizontal scale factor
    if ((factor < 1.0 && current < kZoomMin) ||
        (factor > 1.0 && current > kZoomMax))
        return;

    scale(factor, factor);
}

// ── Pan (middle-mouse or Space + left-drag) ───────────────────────────────────

void GraphView::mousePressEvent(QMouseEvent* event)
{
    const bool wantPan = (event->button() == Qt::MiddleButton) ||
                         (event->button() == Qt::LeftButton && m_spaceHeld);
    if (wantPan) {
        m_panning    = true;
        m_lastPanPos = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void GraphView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panning) {
        const QPoint delta = event->pos() - m_lastPanPos;
        m_lastPanPos = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value()   - delta.y());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void GraphView::mouseReleaseEvent(QMouseEvent* event)
{
    const bool wasPanning = m_panning &&
                            (event->button() == Qt::MiddleButton ||
                             (event->button() == Qt::LeftButton && m_spaceHeld));
    if (wasPanning) {
        m_panning = false;
        setCursor(m_spaceHeld ? Qt::OpenHandCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void GraphView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = true;
        setCursor(Qt::OpenHandCursor);
    }
    QGraphicsView::keyPressEvent(event);
}

void GraphView::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = false;
        if (!m_panning)
            setCursor(Qt::ArrowCursor);
    }
    QGraphicsView::keyReleaseEvent(event);
}

// ── Grid background ───────────────────────────────────────────────────────────

void GraphView::drawBackground(QPainter* painter, const QRectF& rect)
{
    QGraphicsView::drawBackground(painter, rect);

    // Minor grid
    painter->setPen(QPen(QColor(218, 218, 226), 0));   // cosmetic pen = 1 device pixel
    const int minor = 25;
    const qreal left   = std::floor(rect.left()   / minor) * minor;
    const qreal top    = std::floor(rect.top()    / minor) * minor;

    for (qreal x = left;  x <= rect.right();  x += minor)
        painter->drawLine(QLineF(x, rect.top(), x, rect.bottom()));
    for (qreal y = top;   y <= rect.bottom(); y += minor)
        painter->drawLine(QLineF(rect.left(), y, rect.right(), y));

    // Major grid (every 5 minor cells)
    painter->setPen(QPen(QColor(195, 195, 208), 0));
    const int major = minor * 5;
    const qreal mleft = std::floor(rect.left()  / major) * major;
    const qreal mtop  = std::floor(rect.top()   / major) * major;

    for (qreal x = mleft; x <= rect.right();  x += major)
        painter->drawLine(QLineF(x, rect.top(), x, rect.bottom()));
    for (qreal y = mtop;  y <= rect.bottom(); y += major)
        painter->drawLine(QLineF(rect.left(), y, rect.right(), y));
}
