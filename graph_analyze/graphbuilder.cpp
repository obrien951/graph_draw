#include "graphbuilder.h"
#include "ir.h"
#include "graphscene.h"
#include "graphnode.h"
#include "graphedge.h"
#include "labelresolver.h"

#include <QHash>
#include <QSet>
#include <QVector>

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

    // ── Resolve ambiguous type labels (B8) ───────────────────────────────────
    // Two Type entries can legitimately share a bare label (two Rust crates
    // each with their own `Error`, or two C++ classes of the same name in
    // different directories). Candidates are tried bare-first, so a type
    // whose label is unique across the whole repo always resolves to its own
    // bare label unchanged — this block is a no-op for every repo that has no
    // collisions, which today is every existing fixture and the committed
    // graphs/graph_draw.generated.json.
    QVector<graph_lang::LabelResolver::Item> typeItems;
    typeItems.reserve(repo.types().size());
    for (const ir::Type& t : repo.types()) {
        graph_lang::LabelResolver::Item item;
        item.name = t.label;
        QString qualified = t.moduleLabel.isEmpty()
                                 ? QString()
                                 : t.moduleLabel + QStringLiteral("::") + t.label;
        if (qualified == t.label) qualified.clear();
        item.candidates = { t.label, qualified };
        typeItems.append(item);
    }
    graph_lang::LabelResolver typeResolver;
    typeResolver.observe(typeItems);

    // resolvedTypeLabel[i] is the label repo.types().at(i) actually gets as a
    // node. resolvedForBare maps a Type's ORIGINAL bare label to every
    // resolved label it produced, so a bare edge endpoint (baseLabels/
    // usesLabels carry no module context) can still find its target: unique
    // ones resolve exactly as before, and one that's itself ambiguous (two
    // different types both named "Widget" being pointed at by bare "Widget")
    // is dropped rather than guessed at, same philosophy as any other
    // unresolved label in this file.
    QVector<QString> resolvedTypeLabel;
    resolvedTypeLabel.reserve(repo.types().size());
    QHash<QString, QStringList> resolvedForBare;
    for (int i = 0; i < typeItems.size(); ++i) {
        const QString resolved = typeResolver.resolve(typeItems.at(i));
        resolvedTypeLabel.append(resolved);
        QStringList& bucket = resolvedForBare[typeItems.at(i).name];
        if (!bucket.contains(resolved)) bucket.append(resolved);
    }

    // Methods arrive pre-qualified "<bareTypeLabel>::<name>" by the analyzer.
    // When resolution changed a type's label, re-qualify its methods the same
    // way so crateA::Error::new and crateB::Error::new don't collide on the
    // function-node band either. A method label that doesn't match the
    // expected "<bareTypeLabel>::" prefix is left untouched rather than
    // guessed at.
    QVector<QVector<ir::Function>> resolvedMethods;
    resolvedMethods.reserve(repo.types().size());
    for (int i = 0; i < repo.types().size(); ++i) {
        const ir::Type& t = repo.types().at(i);
        const QString& resolved = resolvedTypeLabel.at(i);
        QVector<ir::Function> methods = t.methods;
        if (resolved != t.label) {
            const QString oldPrefix = t.label + QStringLiteral("::");
            const QString newPrefix = resolved + QStringLiteral("::");
            for (ir::Function& f : methods)
                if (f.label.startsWith(oldPrefix))
                    f.label = newPrefix + f.label.mid(oldPrefix.size());
        }
        resolvedMethods.append(methods);
    }

    int ti = 0;
    for (int i = 0; i < repo.types().size(); ++i) {
        const ir::Type& t = repo.types().at(i);
        const QString& label = resolvedTypeLabel.at(i);
        if (typeNode.contains(label)) continue;
        typeNode.insert(label, addNode(NodeType::Class, label, t.description,
                                       ti++, kTypeBand));
    }

    // ── Resolve ambiguous free-function labels (R4.5) ────────────────────────
    // A free function's bare label collides across modules the same way a
    // Type's does — two crates each with `fn main()`, or a crate root and one
    // of its own child modules each with a same-named helper — so it gets the
    // same treatment: bare-first candidates, a no-op for every label that's
    // already unique. Unlike Types, a free function's only inbound edge is
    // "module -> function contains" from its own already-known owning
    // module, so there's no bare-edge-endpoint ambiguity to solve here —
    // just the node-identity collision.
    QVector<graph_lang::LabelResolver::Item> freeFnItems;
    for (const ir::Module& m : repo.modules())
        for (const ir::Function& f : m.freeFunctions) {
            graph_lang::LabelResolver::Item item;
            item.name = f.label;
            QString qualified = m.label.isEmpty()
                                     ? QString()
                                     : m.label + QStringLiteral("::") + f.label;
            if (qualified == f.label) qualified.clear();
            item.candidates = { f.label, qualified };
            freeFnItems.append(item);
        }
    graph_lang::LabelResolver freeFnResolver;
    freeFnResolver.observe(freeFnItems);

    // resolvedFreeFunctions[i] mirrors repo.modules().at(i).freeFunctions,
    // relabeled, so the "module -> function contains" loop below can zip
    // them back together positionally without a second lookup — same shape
    // as resolvedMethods above.
    QVector<QVector<ir::Function>> resolvedFreeFunctions;
    resolvedFreeFunctions.reserve(repo.modules().size());
    {
        int itemIdx = 0;
        for (const ir::Module& m : repo.modules()) {
            QVector<ir::Function> fns;
            fns.reserve(m.freeFunctions.size());
            for (const ir::Function& f : m.freeFunctions) {
                ir::Function rf = f;
                rf.label = freeFnResolver.resolve(freeFnItems.at(itemIdx++));
                fns.append(rf);
            }
            resolvedFreeFunctions.append(fns);
        }
    }

    // Functions come from two places: a type's methods and a module's free
    // functions. Both land in the same band and the same label namespace.
    int fi = 0;
    auto addFunction = [&](const ir::Function& f) {
        if (functionNode.contains(f.label)) return;
        functionNode.insert(f.label, addNode(NodeType::Function, f.label, f.description,
                                             fi++, kFunctionBand));
    };
    for (const QVector<ir::Function>& methods : resolvedMethods)
        for (const ir::Function& f : methods) addFunction(f);
    for (const QVector<ir::Function>& fns : resolvedFreeFunctions)
        for (const ir::Function& f : fns) addFunction(f);

    // Bare label -> GraphNode*, for edge endpoints that carry no module
    // context (baseLabels/usesLabels). Returns null when the bare name is
    // unknown OR itself ambiguous, so such edges are dropped below exactly
    // like any other unresolved label.
    auto typeNodeForBareLabel = [&](const QString& bare) -> GraphNode* {
        const QStringList candidates = resolvedForBare.value(bare);
        if (candidates.size() != 1) return nullptr;
        return typeNode.value(candidates.first());
    };

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

    for (int i = 0; i < repo.modules().size(); ++i) {
        const ir::Module& m = repo.modules().at(i);
        GraphNode* mn = moduleNode.value(m.label);

        // module -> module "contains" (nested modules)
        for (const QString& child : m.childModuleLabels)
            connect(mn, moduleNode.value(child), kContains);

        // module -> function "contains" (free functions; Rust reuses the label
        // rather than introducing a sixth one)
        for (const ir::Function& f : resolvedFreeFunctions.at(i))
            connect(mn, functionNode.value(f.label), kContains);

        // module -> module "depends on"
        for (const QString& dep : m.dependsOnLabels)
            connect(mn, moduleNode.value(dep), kDependsOn);
    }

    for (int i = 0; i < repo.types().size(); ++i) {
        const ir::Type& t = repo.types().at(i);
        GraphNode* tn = typeNode.value(resolvedTypeLabel.at(i));

        // module -> class "contains"
        connect(moduleNode.value(t.moduleLabel), tn, kContains);

        // class -> function "provides"
        for (const ir::Function& f : resolvedMethods.at(i))
            connect(tn, functionNode.value(f.label), kProvides);

        // class -> class "inherits"
        for (const QString& base : t.baseLabels)
            connect(tn, typeNodeForBareLabel(base), kInherits);

        // class -> class "uses"
        for (const QString& use : t.usesLabels)
            connect(tn, typeNodeForBareLabel(use), kUses);
    }
}
