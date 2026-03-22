#pragma once
#include <QGraphicsView>

class GraphView : public QGraphicsView {
    Q_OBJECT
public:
    explicit GraphView(QWidget* parent = nullptr);

protected:
    void wheelEvent(QWheelEvent* event)        override;
    void mousePressEvent(QMouseEvent* event)   override;
    void mouseMoveEvent(QMouseEvent* event)    override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event)       override;
    void keyReleaseEvent(QKeyEvent* event)     override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;

private:
    bool   m_panning    = false;
    bool   m_spaceHeld  = false;
    QPoint m_lastPanPos;

    static constexpr double kZoomStep = 1.15;
    static constexpr double kZoomMin  = 0.05;
    static constexpr double kZoomMax  = 12.0;
};
