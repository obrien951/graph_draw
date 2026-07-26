#include "graphscene.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGraphicsSceneMouseEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>

GraphScene::GraphScene(QObject* parent)
    : QGraphicsScene(parent)
{
    // Fix a large scene rect so the scrollbars don't jump as items are added
    setSceneRect(-4000, -4000, 8000, 8000);
}

// ── Mode management ───────────────────────────────────────────────────────────

void GraphScene::setMode(EditMode mode)
{
    if (m_mode == mode)
        return;
    cancelEdgeCreation();
    m_mode = mode;
    emit modeChanged(mode);
}

// ── Safe deletion ─────────────────────────────────────────────────────────────

void GraphScene::deleteEdge(GraphEdge* edge)
{
    if (!edge) return;
    removeItem(edge);
    delete edge;
}

void GraphScene::deleteNode(GraphNode* node)
{
    if (!node) return;

    // Collect incident edges into a snapshot; deleting one edge modifies the
    // node's registry list so we must not iterate it while deleting.
    const QList<QPointer<GraphEdge>> snapshot = node->edges();
    for (const QPointer<GraphEdge>& edgePtr : snapshot) {
        if (GraphEdge* edge = edgePtr.data())
            deleteEdge(edge);
    }

    removeItem(node);
    delete node;
}

void GraphScene::clearAll()
{
    // Delete in safe order: edges first so no node pointer ever dangles inside
    // a live GraphEdge, then nodes.
    const QList<QGraphicsItem*> all = items();
    for (QGraphicsItem* item : all) {
        if (qgraphicsitem_cast<GraphEdge*>(item)) {
            removeItem(item);
            delete item;
        }
    }
    for (QGraphicsItem* item : items()) {
        if (qgraphicsitem_cast<GraphNode*>(item)) {
            removeItem(item);
            delete item;
        }
    }
}

// ── Dependency tracking (graph-wide) ────────────────────────────────────────────

QList<GraphNode*> GraphScene::nodes() const
{
    QList<GraphNode*> result;
    for (QGraphicsItem* item : items())
        if (auto* n = qgraphicsitem_cast<GraphNode*>(item))
            result.append(n);
    return result;
}

QList<GraphNode*> GraphScene::readyToImplement() const
{
    QList<GraphNode*> result;
    for (GraphNode* n : nodes()) {
        if (n->isImplemented())
            continue;
        // Ready or leaf (no dependencies) — nothing blocks implementation.
        if (n->dependencyStatus() != DependencyStatus::Blocked)
            result.append(n);
    }
    return result;
}

QList<GraphNode*> GraphScene::blocked() const
{
    QList<GraphNode*> result;
    for (GraphNode* n : nodes()) {
        if (n->isImplemented())
            continue;
        if (n->dependencyStatus() == DependencyStatus::Blocked)
            result.append(n);
    }
    return result;
}

// ── Item lookup ───────────────────────────────────────────────────────────────

GraphNode* GraphScene::nodeAt(const QPointF& scenePos) const
{
    // Iterate front-to-back; skip the temp rubber-band line
    for (QGraphicsItem* item : items(scenePos)) {
        if (auto* node = qgraphicsitem_cast<GraphNode*>(item))
            return node;
    }
    return nullptr;
}

GraphEdge* GraphScene::edgeAt(const QPointF& scenePos) const
{
    for (QGraphicsItem* item : items(scenePos)) {
        if (auto* edge = qgraphicsitem_cast<GraphEdge*>(item))
            return edge;
    }
    return nullptr;
}

// ── Node creation ─────────────────────────────────────────────────────────────

void GraphScene::addNodeAt(NodeType kind, const QPointF& pos)
{
    const QString typeName = (kind == NodeType::Class)    ? QStringLiteral("Class")
                           : (kind == NodeType::Module)   ? QStringLiteral("Module")
                                                          : QStringLiteral("Function");
    bool ok = false;
    const QString name = QInputDialog::getText(
        nullptr,
        QStringLiteral("Add ") + typeName,
        QStringLiteral("Name:"),
        QLineEdit::Normal,
        typeName,
        &ok);

    if (ok && !name.trimmed().isEmpty()) {
        auto* node = new GraphNode(kind, name.trimmed());
        node->setPos(pos);
        addItem(node);
    }
}

// ── Edge creation state machine ───────────────────────────────────────────────

void GraphScene::handleEdgeClick(const QPointF& scenePos)
{
    GraphNode* clicked = nodeAt(scenePos);

    if (!m_edgeSrc) {
        // Phase 1: waiting for source node
        if (!clicked) return;
        m_edgeSrc = clicked;

        m_tempLine = new QGraphicsLineItem(
            QLineF(clicked->mapToScene(QPointF(0, 0)), scenePos));
        m_tempLine->setPen(QPen(QColor(120, 120, 120), 1.5, Qt::DashLine));
        addItem(m_tempLine);
    } else {
        // Phase 2: waiting for target node
        if (clicked && clicked != m_edgeSrc) {
            bool ok = false;
            const QString label = QInputDialog::getText(
                nullptr,
                QStringLiteral("Edge Label"),
                QStringLiteral("Label (optional):"),
                QLineEdit::Normal,
                QString(),
                &ok);

            if (ok) {
                auto* edge = new GraphEdge(m_edgeSrc, clicked, label.trimmed());
                addItem(edge);
            }
        }
        cancelEdgeCreation();
    }
}

void GraphScene::cancelEdgeCreation()
{
    if (m_tempLine) {
        removeItem(m_tempLine);
        delete m_tempLine;
        m_tempLine = nullptr;
    }
    m_edgeSrc = nullptr;
}

// ── Event handlers ────────────────────────────────────────────────────────────

void GraphScene::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QGraphicsScene::mousePressEvent(event);
        return;
    }

    switch (m_mode) {
    case EditMode::AddClass:    addNodeAt(NodeType::Class,    event->scenePos()); break;
    case EditMode::AddModule:   addNodeAt(NodeType::Module,   event->scenePos()); break;
    case EditMode::AddFunction: addNodeAt(NodeType::Function, event->scenePos()); break;
    case EditMode::AddEdge:     handleEdgeClick(event->scenePos());               break;

    case EditMode::Delete: {
        // Prefer deleting a node over an edge when both are under the cursor
        if (GraphNode* node = nodeAt(event->scenePos()))
            deleteNode(node);
        else if (GraphEdge* edge = edgeAt(event->scenePos()))
            deleteEdge(edge);
        break;
    }

    case EditMode::Select:
        QGraphicsScene::mousePressEvent(event);
        break;
    }
}

void GraphScene::mouseMoveEvent(QGraphicsSceneMouseEvent* event)
{
    // Update the rubber-band line while the user is picking a target node
    if (m_mode == EditMode::AddEdge && m_tempLine && m_edgeSrc) {
        QLineF line = m_tempLine->line();
        line.setP2(event->scenePos());
        m_tempLine->setLine(line);
    }
    QGraphicsScene::mouseMoveEvent(event);
}

void GraphScene::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event)
{
    if (m_mode != EditMode::Select) {
        QGraphicsScene::mouseDoubleClickEvent(event);
        return;
    }

    const QPointF pos = event->scenePos();

    if (GraphNode* node = nodeAt(pos)) {
        QDialog dlg;
        dlg.setWindowTitle(QStringLiteral("Edit Node"));

        auto* nameEdit = new QLineEdit(node->label());

        // The description is the implementation prompt for the module/class/
        // function this node represents, so it needs room for multi-line text.
        auto* descEdit = new QPlainTextEdit(node->comment());
        descEdit->setPlaceholderText(
            QStringLiteral("Prompt / description used to implement this node"));
        descEdit->setMinimumSize(360, 140);

        // Lets the user mark whether the thing this node names is implemented.
        // Dependency tracking reads this flag across the graph.
        auto* implementedCheck = new QCheckBox(QStringLiteral("Implemented"));
        implementedCheck->setChecked(node->isImplemented());

        auto* form = new QFormLayout;
        form->addRow(QStringLiteral("Name:"),        nameEdit);
        form->addRow(QStringLiteral("Description:"), descEdit);
        form->addRow(QString(),                      implementedCheck);

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        auto* layout = new QVBoxLayout(&dlg);
        layout->addLayout(form);
        layout->addWidget(buttons);

        if (dlg.exec() == QDialog::Accepted) {
            if (!nameEdit->text().trimmed().isEmpty())
                node->setLabel(nameEdit->text().trimmed());
            node->setComment(descEdit->toPlainText().trimmed());
            node->setImplemented(implementedCheck->isChecked());
        }
        return;
    }

    if (GraphEdge* edge = edgeAt(pos)) {
        bool ok = false;
        const QString label = QInputDialog::getText(
            nullptr,
            QStringLiteral("Edit Edge Label"),
            QStringLiteral("Label:"),
            QLineEdit::Normal,
            edge->label(),
            &ok);
        if (ok)
            edge->setLabel(label.trimmed());
        return;
    }

    QGraphicsScene::mouseDoubleClickEvent(event);
}

void GraphScene::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        // Snapshot selected items before any deletion modifies the scene
        const QList<QGraphicsItem*> selected = selectedItems();
        for (QGraphicsItem* item : selected) {
            if (auto* node = qgraphicsitem_cast<GraphNode*>(item))
                deleteNode(node);
            else if (auto* edge = qgraphicsitem_cast<GraphEdge*>(item))
                deleteEdge(edge);
        }
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        cancelEdgeCreation();
        return;
    }

    QGraphicsScene::keyPressEvent(event);
}
