#include "ir/ir.h"

namespace graph_lang {
namespace ir {

Module::Module(const QString& label, const QString& description)
    : m_id(0), m_label(label), m_description(description) {}

Module::~Module() = default;

QString Module::label() const { return m_label; }
void Module::setLabel(const QString& label) { m_label = label; }

QString Module::description() const { return m_description; }
void Module::setDescription(const QString& description) { m_description = description; }

QStringList Module::dependsOnLabels() const { return m_dependsOnLabels; }
void Module::setDependsOnLabels(const QStringList& labels) { m_dependsOnLabels = labels; }

QStringList Module::childModuleLabels() const { return m_childModuleLabels; }
void Module::addChildModuleLabels(const QStringList& labels) { m_childModuleLabels = labels; }

QStringList Module::freeFunctionLabels() const { return m_freeFunctionLabels; }
void Module::setFreeFunctionLabels(const QStringList& labels) { m_freeFunctionLabels = labels; }

int Module::id() const { return m_id; }
void Module::setId(int id) { m_id = id; }

Type::Type(const QString& label, const QString& description)
    : m_id(0), m_label(label), m_description(description) {}

Type::~Type() = default;

QString Type::label() const { return m_label; }
void Type::setLabel(const QString& label) { m_label = label; }

QString Type::description() const { return m_description; }
void Type::setDescription(const QString& description) { m_description = description; }

QString Type::owningModuleLabel() const { return m_owningModuleLabel; }
void Type::setOwningModuleLabel(const QString& label) { m_owningModuleLabel = label; }

QStringList Type::baseLabels() const { return m_baseLabels; }
void Type::setBaseLabels(const QStringList& labels) { m_baseLabels = labels; }

QStringList Type::usesLabels() const { return m_usesLabels; }
void Type::setUsesLabels(const QStringList& labels) { m_usesLabels = labels; }

QStringList Type::methodLabels() const { return m_methodLabels; }
void Type::setMethodLabels(const QStringList& labels) { m_methodLabels = labels; }

int Type::id() const { return m_id; }
void Type::setId(int id) { m_id = id; }

Function::Function(const QString& label, const QString& description)
    : m_id(0), m_label(label), m_description(description) {}

Function::~Function() = default;

QString Function::label() const { return m_label; }
void Function::setLabel(const QString& label) { m_label = label; }

QString Function::description() const { return m_description; }
void Function::setDescription(const QString& description) { m_description = description; }

int Function::id() const { return m_id; }
void Function::setId(int id) { m_id = id; }

Repo::Repo() : m_id(0), m_nextId(1) {}

Repo::~Repo() = default;

int Repo::nextId() const { return m_nextId++; }

void Repo::addModule(std::unique_ptr<Module> module) {
    module->setId(m_nextId++);
    m_modules.append(std::move(module));
}

void Repo::addType(std::unique_ptr<Type> type) {
    type->setId(m_nextId++);
    m_types.append(std::move(type));
}

void Repo::addFunction(std::unique_ptr<Function> function) {
    function->setId(m_nextId++);
    m_functions.append(std::move(function));
}

int Repo::moduleCount() const { return m_modules.size(); }
int Repo::typeCount() const { return m_types.size(); }
int Repo::functionCount() const { return m_functions.size(); }

const Module* Repo::module(int index) const {
    if (index < 0 || index >= static_cast<int>(m_modules.size()))
        return nullptr;
    return m_modules[index].get();
}

const Type* Repo::type(int index) const {
    if (index < 0 || index >= static_cast<int>(m_types.size()))
        return nullptr;
    return m_types[index].get();
}

const Function* Repo::function(int index) const {
    if (index < 0 || index >= static_cast<int>(m_functions.size()))
        return nullptr;
    return m_functions[index].get();
}

QStringList Repo::moduleLabels() const {
    QStringList labels;
    for (const auto& mod : m_modules) {
        labels.append(mod->label());
    }
    return labels;
}

QStringList Repo::typeLabels() const {
    QStringList labels;
    for (const auto& t : m_types) {
        labels.append(t->label());
    }
    return labels;
}

QStringList Repo::functionLabels() const {
    QStringList labels;
    for (const auto& f : m_functions) {
        labels.append(f->label());
    }
    return labels;
}

int Repo::id() const { return m_id; }
void Repo::setId(int id) { m_id = id; }

} // namespace ir
} // namespace graph_lang
