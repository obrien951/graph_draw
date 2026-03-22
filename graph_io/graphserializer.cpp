#include "graphserializer.h"

// ── Stub implementations ──────────────────────────────────────────────────────
//
// These will be replaced by real JSON serialisation once the graph model is
// stable.  The Qt JSON API (QJsonDocument, QJsonObject, QJsonArray) requires
// no additional dependencies and will be the implementation vehicle.

bool GraphSerializer::saveToFile(const QString& /*filePath*/,
                                 const GraphScene* /*scene*/)
{
    m_lastError = QStringLiteral("Save is not yet implemented.");
    return false;
}

bool GraphSerializer::loadFromFile(const QString& /*filePath*/,
                                   GraphScene* /*scene*/)
{
    m_lastError = QStringLiteral("Load is not yet implemented.");
    return false;
}
