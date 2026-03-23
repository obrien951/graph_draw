#include "graphserializer.h"
#include "graphscene.h"
#include "graphnode.h"
#include "graphedge.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <optional>

// ── Helpers ───────────────────────────────────────────────────────────────────

static QString kindToString(NodeType kind)
{
    switch (kind) {
    case NodeType::Module:   return QStringLiteral("Module");
    case NodeType::Class:    return QStringLiteral("Class");
    case NodeType::Function: return QStringLiteral("Function");
    }
    return QStringLiteral("Class");
}

static std::optional<NodeType> kindFromString(const QString& s)
{
    if (s == QLatin1String("Module"))   return NodeType::Module;
    if (s == QLatin1String("Class"))    return NodeType::Class;
    if (s == QLatin1String("Function")) return NodeType::Function;
    return std::nullopt;
}

// ── Save ──────────────────────────────────────────────────────────────────────

bool GraphSerializer::saveToFile(const QString& filePath, const GraphScene* scene)
{
    m_lastError.clear();

    // Collect nodes and edges from the scene, preserving a stable order so
    // edge indices into the node list are deterministic.
    QList<GraphNode*> nodes;
    QList<GraphEdge*> edges;
    for (QGraphicsItem* item : scene->items()) {
        if (auto* n = qgraphicsitem_cast<GraphNode*>(item)) nodes.append(n);
        if (auto* e = qgraphicsitem_cast<GraphEdge*>(item)) edges.append(e);
    }

    QJsonArray nodeArray;
    for (const GraphNode* node : nodes) {
        QJsonObject position;
        position[QStringLiteral("x")] = node->pos().x();
        position[QStringLiteral("y")] = node->pos().y();

        QJsonObject obj;
        obj[QStringLiteral("kind")]     = kindToString(node->kind());
        obj[QStringLiteral("name")]     = node->label();
        obj[QStringLiteral("position")] = position;
        nodeArray.append(obj);
    }

    QJsonArray edgeArray;
    for (const GraphEdge* edge : edges) {
        const int originIdx      = nodes.indexOf(edge->source());
        const int destinationIdx = nodes.indexOf(edge->target());
        if (originIdx < 0 || destinationIdx < 0) {
            m_lastError = QStringLiteral("Edge references a node not present in the scene.");
            return false;
        }

        QJsonObject obj;
        obj[QStringLiteral("origin")]      = originIdx;
        obj[QStringLiteral("destination")] = destinationIdx;
        obj[QStringLiteral("comment")]     = edge->label();
        edgeArray.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("nodes")] = nodeArray;
    root[QStringLiteral("edges")] = edgeArray;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastError = QStringLiteral("Cannot open file for writing: ") + file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson());
    return true;
}

// ── Load ──────────────────────────────────────────────────────────────────────

bool GraphSerializer::loadFromFile(const QString& filePath, GraphScene* scene)
{
    m_lastError.clear();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("Cannot open file for reading: ") + file.errorString();
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        m_lastError = QStringLiteral("JSON parse error: ") + parseError.errorString();
        return false;
    }

    const QJsonObject root = doc.object();
    if (!root.contains(QLatin1String("nodes")) || !root.contains(QLatin1String("edges"))) {
        m_lastError = QStringLiteral("File is missing required 'nodes' or 'edges' fields.");
        return false;
    }

    scene->clearAll();

    QList<GraphNode*> nodes;
    for (const QJsonValue& val : root[QStringLiteral("nodes")].toArray()) {
        const QJsonObject obj = val.toObject();
        const auto kind = kindFromString(obj[QStringLiteral("kind")].toString());
        if (!kind) {
            m_lastError = QStringLiteral("Unknown node kind: ") +
                          obj[QStringLiteral("kind")].toString();
            return false;
        }
        const QJsonObject pos = obj[QStringLiteral("position")].toObject();
        auto* node = new GraphNode(*kind, obj[QStringLiteral("name")].toString());
        node->setPos(pos[QStringLiteral("x")].toDouble(),
                     pos[QStringLiteral("y")].toDouble());
        scene->addItem(node);
        nodes.append(node);
    }

    for (const QJsonValue& val : root[QStringLiteral("edges")].toArray()) {
        const QJsonObject obj  = val.toObject();
        const int originIdx      = obj[QStringLiteral("origin")].toInt(-1);
        const int destinationIdx = obj[QStringLiteral("destination")].toInt(-1);

        if (originIdx < 0 || originIdx >= nodes.size() ||
            destinationIdx < 0 || destinationIdx >= nodes.size()) {
            m_lastError = QStringLiteral("Edge references an out-of-range node index.");
            return false;
        }

        auto* edge = new GraphEdge(nodes[originIdx], nodes[destinationIdx],
                                   obj[QStringLiteral("comment")].toString());
        scene->addItem(edge);
    }

    return true;
}
