#pragma once
#include <QString>
#include <QStringList>

// rustlex::leadingDoc: Doc comment above an item, skipping blank AND #[attribute] lines.
// The dominant real shape is: /// line, then #[derive(...)], then the item.
// Strips 3 characters for /// and 2 for //! (not 2 for /// as a common mistake).
// Returns the stripped comment text or empty string if none found.
QString leadingDoc(const QStringList& rawLines, int declLine);
