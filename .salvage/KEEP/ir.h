#ifndef GRAPH_LANG_IR_IR_H
#define GRAPH_LANG_IR_IR_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>
#include <QSet>
#include <memory>

namespace graph_lang {
namespace ir {

/**
 * @brief A Module node in the IR.
 * Covers a CMake target, a directory fallback, a Cargo crate, and a Rust module.
 */
class Module {
public:
    Module(const QString& label, const QString& description = QString());
    ~Module();

    QString label() const;
    void setLabel(const QString& label);

    QString description() const;
    void setDescription(const QString& description);

    QStringList dependsOnLabels() const;
    void setDependsOnLabels(const QStringList& labels);

    QStringList childModuleLabels() const;
    void addChildModuleLabels(const QStringList& labels);

    QStringList freeFunctionLabels() const;
    void setFreeFunctionLabels(const QStringList& labels);

    // For JSON serialization / ordering
    int id() const;
    void setId(int id);

private:
    int m_id;
    QString m_label;
    QString m_description;
    QStringList m_dependsOnLabels;
    QStringList m_childModuleLabels;
    QStringList m_freeFunctionLabels;
};

/**
 * @brief A Class node in the IR.
 * Covers a C++ class/struct and a Rust struct/enum/trait/union.
 */
class Type {
public:
    Type(const QString& label, const QString& description = QString());
    ~Type();

    QString label() const;
    void setLabel(const QString& label);

    QString description() const;
    void setDescription(const QString& description);

    QString owningModuleLabel() const;
    void setOwningModuleLabel(const QString& label);

    QStringList baseLabels() const;
    void setBaseLabels(const QStringList& labels);

    QStringList usesLabels() const;
    void setUsesLabels(const QStringList& labels);

    QStringList methodLabels() const;
    void setMethodLabels(const QStringList& labels);

    // For JSON serialization / ordering
    int id() const;
    void setId(int id);

private:
    int m_id;
    QString m_label;
    QString m_description;
    QString m_owningModuleLabel;
    QStringList m_baseLabels;
    QStringList m_usesLabels;
    QStringList m_methodLabels;
};

/**
 * @brief A Function node in the IR.
 * Descriptions live in the IR rather than being formatted by the builder.
 */
class Function {
public:
    Function(const QString& label, const QString& description = QString());
    ~Function();

    QString label() const;
    void setLabel(const QString& label);

    QString description() const;
    void setDescription(const QString& description);

    // For JSON serialization / ordering
    int id() const;
    void setId(int id);

private:
    int m_id;
    QString m_label;
    QString m_description;
};

/**
 * @brief A repository's worth of nodes.
 * A list of ir::Module and a list of ir::Type, contributed by one or more analyzers.
 * Ordering is node creation order and therefore JSON array order.
 */
class Repo {
public:
    Repo();
    ~Repo();

    int nextId() const;

    void addModule(std::unique_ptr<Module> module);
    void addType(std::unique_ptr<Type> type);
    void addFunction(std::unique_ptr<Function> function);

    int moduleCount() const;
    int typeCount() const;
    int functionCount() const;

    const Module* module(int index) const;
    const Type* type(int index) const;
    const Function* function(int index) const;

    QStringList moduleLabels() const;
    QStringList typeLabels() const;
    QStringList functionLabels() const;

    // For JSON serialization
    int id() const;
    void setId(int id);

private:
    int m_id;
    int m_nextId;
    QVector<std::unique_ptr<Module>> m_modules;
    QVector<std::unique_ptr<Type>> m_types;
    QVector<std::unique_ptr<Function>> m_functions;
};

} // namespace ir
} // namespace graph_lang

#endif // GRAPH_LANG_IR_IR_H
