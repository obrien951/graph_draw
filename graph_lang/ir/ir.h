#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

// The language-neutral intermediate representation (B2).
//
// Analyzers produce an ir::Repo; GraphBuilder turns one into a GraphScene. That
// split is what keeps graph_lang free of graph_core: a language plugin describes
// what it found, and something above it decides how that becomes scene items.
//
// These are plain values, deliberately. Ordering is creation order and therefore
// JSON array order, so an analyzer that appends deterministically produces a
// deterministic graph.
//
// All cross-references are by LABEL, not index or pointer. Labels that never
// resolve are silently dropped by the builder, which reproduces the existing
// "only link a base class that is itself a node" guard without special-casing it.
namespace ir {

// A Function node: a class method, or a free function owned by a module.
//
// The description lives here rather than being formatted by the builder. Today
// the function description is built inside the scene phase, which is precisely
// why ModuleInfo/ClassInfo are not quite language-neutral as they stand.
struct Function {
    QString label;
    QString description;
};

// A Class node. Covers a C++ class/struct and a Rust struct/enum/trait/union.
struct Type {
    QString           label;
    QString           description;
    QString           moduleLabel;    // owning module, by label
    QStringList       baseLabels;     // -> "inherits"
    QStringList       usesLabels;     // -> "uses"
    QVector<Function> methods;        // -> "provides"
};

// A Module node. Covers a CMake target, a directory fallback, a Cargo crate, and
// a Rust module.
struct Module {
    QString           label;
    QString           description;
    QStringList       dependsOnLabels;    // -> "depends on"
    QStringList       childModuleLabels;  // -> "contains" (nested modules)
    QVector<Function> freeFunctions;      // -> "contains" (Rust reuses the label)
};

// One repository's worth of nodes, contributed by one or more analyzers.
//
// Analyzers APPEND; nothing here ever clears, so a polyglot repository is just
// two analyzers writing into the same Repo.
class Repo {
public:
    void addModule(const Module& m) { m_modules.append(m); }
    void addType(const Type& t)     { m_types.append(t); }

    const QVector<Module>& modules() const { return m_modules; }
    const QVector<Type>&   types() const   { return m_types; }

    // Mutable access for analyzers that fill a node in several passes (the C++
    // analyzer resolves owning modules only after every file has been scanned).
    QVector<Module>& modules() { return m_modules; }
    QVector<Type>&   types()   { return m_types; }

    bool isEmpty() const { return m_modules.isEmpty() && m_types.isEmpty(); }

    // Label lookups, used by analyzers to avoid emitting duplicates and by the
    // builder to resolve edge endpoints.
    bool hasModule(const QString& label) const;
    bool hasType(const QString& label) const;
    const Module* module(const QString& label) const;
    const Type*   type(const QString& label) const;

    QStringList moduleLabels() const;
    QStringList typeLabels() const;

private:
    QVector<Module> m_modules;
    QVector<Type>   m_types;
};

} // namespace ir
