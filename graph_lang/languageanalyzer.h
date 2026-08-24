#pragma once
#include <QString>

class RepoFileIndex;
namespace ir { class Repo; }

// The seam every language plugin implements (B1).
//
// Note what is absent: no GraphScene, no QGraphicsItem, no graph_io. graph_lang
// links Qt Core ONLY, so an analyzer structurally cannot build scene items and
// its tests need no QApplication. Turning an ir::Repo into a scene is
// GraphBuilder's job and happens once, above this layer.
class LanguageAnalyzer {
public:
    virtual ~LanguageAnalyzer();

    // Stable short id used by --lang and in messages: "cpp", "rust".
    virtual QString id() const = 0;

    // Cheap, pure decision made from the already-built index, with no
    // filesystem access of its own.
    virtual bool detect(const RepoFileIndex& index) const = 0;

    // APPEND this language's modules, types and functions into repo — never
    // clear it, so several analyzers can contribute to one graph. Returns false
    // and sets *error (when error is non-null) on failure. Must produce a
    // deterministic order.
    virtual bool analyze(const RepoFileIndex& index, ir::Repo* repo,
                         QString* error) const = 0;
};
