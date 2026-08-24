#include "ir.h"

namespace ir {

bool Repo::hasModule(const QString& label) const
{
    return module(label) != nullptr;
}

bool Repo::hasType(const QString& label) const
{
    return type(label) != nullptr;
}

const Module* Repo::module(const QString& label) const
{
    for (const Module& m : m_modules)
        if (m.label == label) return &m;
    return nullptr;
}

const Type* Repo::type(const QString& label) const
{
    for (const Type& t : m_types)
        if (t.label == label) return &t;
    return nullptr;
}

QStringList Repo::moduleLabels() const
{
    QStringList out;
    out.reserve(m_modules.size());
    for (const Module& m : m_modules) out.append(m.label);
    return out;
}

QStringList Repo::typeLabels() const
{
    QStringList out;
    out.reserve(m_types.size());
    for (const Type& t : m_types) out.append(t.label);
    return out;
}

} // namespace ir
