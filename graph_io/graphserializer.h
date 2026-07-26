#pragma once
#include <QString>

class GraphScene;

// GraphSerializer provides save and load operations for a GraphScene.
//
// Format is JSON: nodes are stored as an array of objects with kind, name,
// comment (the implementation prompt/description), an `implemented` flag, and
// position; edges reference nodes by index and carry an optional comment.
// The `implemented` flag is optional on load and defaults to false, so graphs
// written before dependency tracking still load cleanly.
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
