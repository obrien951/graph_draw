#pragma once
#include "graphnode.h"
#include "graphedge.h"
#include <QGraphicsScene>
#include <QGraphicsLineItem>

enum class EditMode {
    Select,
    AddClass,
    AddModule,
    AddFunction,
    AddEdge,
    Delete
};

class GraphScene : public QGraphicsScene {
    Q_OBJECT
public:
    explicit GraphScene(QObject* parent = nullptr);

    void setMode(EditMode mode);
    EditMode mode() const { return m_mode; }

    // Safe deletion helpers — always remove incident edges before the node
    // so no live GraphEdge ever holds a dangling GraphNode pointer.
    void deleteNode(GraphNode* node);
    void deleteEdge(GraphEdge* edge);

    // Removes all items in safe order (edges first, then nodes)
    void clearAll();

    // ── Dependency tracking (graph-wide) ────────────────────────────────────
    // All GraphNodes currently in the scene.
    QList<GraphNode*> nodes() const;

    // Unimplemented nodes whose every dependency is already implemented — the
    // nodes that are safe to implement next without stubbing anything out.
    // (Leaf nodes with no dependencies are included.)
    QList<GraphNode*> readyToImplement() const;

    // Unimplemented nodes that still have at least one unimplemented dependency.
    QList<GraphNode*> blocked() const;

signals:
    void modeChanged(EditMode mode);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event)       override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event)        override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event)                        override;

private:
    // Returns the topmost GraphNode at scenePos, or nullptr
    GraphNode* nodeAt(const QPointF& scenePos) const;
    // Returns the topmost GraphEdge at scenePos, or nullptr
    GraphEdge* edgeAt(const QPointF& scenePos) const;

    void addNodeAt(NodeType kind, const QPointF& pos);
    void handleEdgeClick(const QPointF& scenePos);
    void cancelEdgeCreation();

    EditMode           m_mode       = EditMode::Select;
    GraphNode*         m_edgeSrc    = nullptr;   // non-owning; first click in AddEdge mode
    QGraphicsLineItem* m_tempLine   = nullptr;   // rubber-band visual while drawing an edge
};
