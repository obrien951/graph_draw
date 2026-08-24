#include "graphbuilder.h"
#include "ir.h"
#include "graphscene.h"
#include "graphnode.h"
#include "graphedge.h"

#include <QHash>
#include <QSet>

namespace {

// Layout: fixed horizontal bands, one per kind, wrapping every 12 nodes. Taken
// from the original scanner so the generated graph keeps the same shape.
//
// Known limitation (B12): the bands overlap once a band exceeds ~36 nodes.
void place(GraphNode* n, int index, qreal bandY)
{
    n->setPos((index % 12) * 180.0, bandY + (index / 12) * 130.0);
}

constexpr qreal kModuleBand   = 0.0;
constexpr qreal kTypeBand     = 320.0;
constexpr qreal kFunctionBand = 640.0;

} // namespace

void GraphBuilder::build(const ir::Repo& repo, GraphScene* scene) const
{
    if (!scene) return;
    scene->clearAll();

    QHash<QString, GraphNode*> moduleNode;
    QHash<QString, GraphNode*> typeNode;
    QHash<QString, GraphNode*> functionNode;

    auto addNode = [&](NodeType kind, const QString& label, const QString& description,
                       int index, qreal band) {
        auto* n = new GraphNode(kind, label);
        n->setComment(description);
        // Every node produced from an IR reflects code that exists.
        n->setImplemented(true);
        place(n, index, band);
        scene->addItem(n);
        return n;
    };

    // ── Nodes, in IR order: modules, then types, then functions ─────────────
    // First definition of a label wins, mirroring the original classByName
    // behaviour, so a duplicate never silently displaces the node edges point at.
    int mi = 0;
    for (const ir::Module& m : repo.modules()) {
        if (moduleNode.contains(m.label)) continue;
        moduleNode.insert(m.label, addNode(NodeType::Module, m.label, m.description,
                                           mi++, kModuleBand));
    }

    int ti = 0;
    for (const ir::Type& t : repo.types()) {
        if (typeNode.contains(t.label)) continue;
        typeNode.insert(t.label, addNode(NodeType::Class, t.label, t.description,
                                         ti++, kTypeBand));
    }

    // Functions come from two places: a type's methods and a module's free
    // functions. Both land in the same band and the same label namespace.
    int fi = 0;
    auto addFunction = [&](const ir::Function& f) {
        if (functionNode.contains(f.label)) return;
        functionNode.insert(f.label, addNode(NodeType::Function, f.label, f.description,
                                             fi++, kFunctionBand));
    };
    for (const ir::Type& t : repo.types())
        for (const ir::Function& f : t.methods) addFunction(f);
    for (const ir::Module& m : repo.modules())
        for (const ir::Function& f : m.freeFunctions) addFunction(f);

    // ── Edges ───────────────────────────────────────────────────────────────
    // An endpoint that resolves to no node is silently dropped. That is what
    // reproduces the original "only link a base class that is itself a node"
    // guard without special-casing it. Self-edges are suppressed.
    QSet<QString> seen;
    auto connect = [&](GraphNode* a, GraphNode* b, const QString& label) {
        if (!a || !b || a == b) return;
        const QString key = a->label() + QLatin1Char('\x1f') + b->label()
                          + QLatin1Char('\x1f') + label;
        if (seen.contains(key)) return;
        seen.insert(key);
        scene->addItem(new GraphEdge(a, b, label));
    };

    static const QString kContains  = QStringLiteral("contains");
    static const QString kProvides  = QStringLiteral("provides");
    static const QString kInherits  = QStringLiteral("inherits");
    static const QString kUses      = QStringLiteral("uses");
    static const QString kDependsOn = QStringLiteral("depends on");

    for (const ir::Module& m : repo.modules()) {
        GraphNode* mn = moduleNode.value(m.label);

        // module -> module "contains" (nested modules)
        for (const QString& child : m.childModuleLabels)
            connect(mn, moduleNode.value(child), kContains);

        // module -> function "contains" (free functions; Rust reuses the label
        // rather than introducing a sixth one)
        for (const ir::Function& f : m.freeFunctions)
            connect(mn, functionNode.value(f.label), kContains);

        // module -> module "depends on"
        for (const QString& dep : m.dependsOnLabels)
            connect(mn, moduleNode.value(dep), kDependsOn);
    }

    for (const ir::Type& t : repo.types()) {
        GraphNode* tn = typeNode.value(t.label);

        // module -> class "contains"
        connect(moduleNode.value(t.moduleLabel), tn, kContains);

        // class -> function "provides"
        for (const ir::Function& f : t.methods)
            connect(tn, functionNode.value(f.label), kProvides);

        // class -> class "inherits"
        for (const QString& base : t.baseLabels)
            connect(tn, typeNode.value(base), kInherits);

        // class -> class "uses"
        for (const QString& use : t.usesLabels)
            connect(tn, typeNode.value(use), kUses);
    }
}
