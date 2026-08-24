#include "leading_doc.h"
#include <QRegularExpression>

QString leadingDoc(const QStringList& rawLines, int declLine)
{
    QStringList collected;
    int i = declLine - 1;

    // Skip blank lines between comment and declaration
    while (i >= 0 && rawLines[i].trimmed().isEmpty()) {
        --i;
    }

    // Collect doc comment lines
    for (; i >= 0; --i) {
        QString t = rawLines[i].trimmed();

        // Skip attribute lines (#[...])
        if (t.startsWith('#') && t.contains('=')) {
            continue;
        }

        // Collect doc comment lines
        if (t.startsWith(QLatin1String("///"))) {
            // Strip 3 characters for ///
            t.remove(0, 3);
            collected.prepend(t.trimmed());
        } else if (t.startsWith(QLatin1String("//!"))) {
            // Strip 2 characters for //!
            t.remove(0, 2);
            collected.prepend(t.trimmed());
        } else if (t.startsWith(QLatin1String("/*"))) {
            // Block comment doc
            t.remove(QRegularExpression(QStringLiteral("^/\\*+|\\*+/$|^\\*+")));
            collected.prepend(t.trimmed());
            if (rawLines[i].trimmed().startsWith(QLatin1String("/*"))) {
                break;
            }
        } else {
            break;
        }
    }

    // Remove leading empty lines from collected
    while (!collected.isEmpty() && collected.first().isEmpty()) {
        collected.removeFirst();
    }

    return collected.join(QLatin1Char(' ')).simplified();
}
