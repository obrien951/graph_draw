#include "rust_file_items.h"

RustFileItems::~RustFileItems() = default;

void RustFileItems::setFilePath(const QString& path) { m_path = path; }

QStringList RustFileItems::allItems() const
{
    QStringList all;
    all.append(m_types);
    all.append(m_implBlocks);
    all.append(m_freeFunctions);
    return all;
}
