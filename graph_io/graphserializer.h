#pragma once
#include <QString>

class GraphScene;

// GraphSerializer provides save and load operations for a GraphScene.
//
// The intended format is JSON: nodes are stored as an array of objects with
// type, label, and position; edges reference nodes by index and carry an
// optional label.  Neither operation is implemented yet — both return false
// with an explanatory error string so callers can degrade gracefully.
class GraphSerializer {
public:
    GraphSerializer() = default;

    // Serialise scene to a JSON file at filePath.
    // Returns true on success.
    bool saveToFile(const QString& filePath, const GraphScene* scene);

    // Deserialise a JSON file into scene, replacing its current contents.
    // Returns true on success.
    bool loadFromFile(const QString& filePath, GraphScene* scene);

    // Human-readable description of the last error, or empty if none.
    QString lastError() const { return m_lastError; }

private:
    QString m_lastError;
};
