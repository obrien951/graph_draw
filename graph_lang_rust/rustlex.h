#pragma once

#include <QString>
#include <QStringList>

namespace rustlex {

QString blank(const QString& source);

// The doc comment sitting directly above the item on line itemLine (0-based).
// Walks up from the item, skipping blank lines and #[attribute] lines (the
// dominant shape is a /// line, then #[derive(...)], then the item), and
// collects the /// and //! doc lines, stripping three characters from each
// (/// and //!, not two). Returns "" when the item carries no doc.
QString leadingDoc(const QStringList& lines, int itemLine);

} // namespace rustlex
