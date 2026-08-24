#include "scan_rust_file.h"
#include "rustlex/blank.h"
#include "rustlex/leading_doc.h"
#include <QRegularExpression>
#include <QSet>

RustFileItems scanRustFile(const QString& filePath, const QString& blankedText)
{
    RustFileItems items;
    items.setFilePath(filePath);

    // Split into lines for leading doc detection
    QStringList rawLines = blankedText.split('\n');

    // Find the inner doc comment (//! at the start of the file)
    int firstNonBlank = -1;
    for (int i = 0; i < rawLines.size(); ++i) {
        if (!rawLines[i].trimmed().isEmpty()) {
            firstNonBlank = i;
            break;
        }
    }
    if (firstNonBlank >= 0 && rawLines[firstNonBlank].trimmed().startsWith(QLatin1String("//!"))) {
        QString doc = rawLines[firstNonBlank].trimmed();
        doc.remove(0, 2);  // strip "//!"
        items.setInnerDoc(doc.trimmed());
    }

    // Blank the text for scanning
    QString scanned = blankedText;

    // Find item depth positions (depth == 0)
    QSet<int> itemPositions;
    int depth = 0;
    for (int i = 0; i < scanned.size(); ++i) {
        if (scanned[i] == '{') {
            ++depth;
        } else if (scanned[i] == '}') {
            --depth;
        }
        if (depth == 0) {
            itemPositions.insert(i);
        }
    }

    // Scan for types at item depth
    QRegularExpression typeRe(
        QStringLiteral("\\b(struct|enum|union|trait|type|mod|use)\\s+([A-Za-z_][A-Za-z0-9_]*)\\b"));
    for (int pos : itemPositions) {
        int matchStart = pos;
        while (matchStart > 0 && (scanned[matchStart - 1].isSpace() || scanned[matchStart - 1] == '\n')) {
            --matchStart;
        }
        QRegularExpressionMatch match = typeRe.match(scanned.mid(matchStart));
        if (match.hasMatch()) {
            QString keyword = match.captured(1);
            QString name = match.captured(2);
            // Skip keywords that are not types
            if (keyword == "mod" || keyword == "use") continue;
            items.m_types.append(name);
        }
    }

    // Scan for impl blocks at item depth
    QRegularExpression implRe(
        QStringLiteral("\\b(impl)\\s+(\\w+)?\\s*<[^>]*>\\s*\\w+\\s*\\{"));
    for (int pos : itemPositions) {
        int matchStart = pos;
        while (matchStart > 0 && (scanned[matchStart - 1].isSpace() || scanned[matchStart - 1] == '\n')) {
            --matchStart;
        }
        QRegularExpressionMatch match = implRe.match(scanned.mid(matchStart));
        if (match.hasMatch()) {
            QString implTarget = match.captured(2);
            if (!implTarget.isEmpty()) {
                items.m_implBlocks.append(implTarget);
            }
        }
    }

    // Scan for free functions at item depth
    QRegularExpression funcRe(
        QStringLiteral("\\b(fn)\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*\\("));
    for (int pos : itemPositions) {
        int matchStart = pos;
        while (matchStart > 0 && (scanned[matchStart - 1].isSpace() || scanned[matchStart - 1] == '\n')) {
            --matchStart;
        }
        QRegularExpressionMatch match = funcRe.match(scanned.mid(matchStart));
        if (match.hasMatch()) {
            QString name = match.captured(2);
            items.m_freeFunctions.append(name);
        }
    }

    // Scan for use paths at item depth
    QRegularExpression useRe(
        QStringLiteral("\\b(use)\\s+(\\w+::\\w+|\\w+)"));
    for (int pos : itemPositions) {
        int matchStart = pos;
        while (matchStart > 0 && (scanned[matchStart - 1].isSpace() || scanned[matchStart - 1] == '\n')) {
            --matchStart;
        }
        QRegularExpressionMatch match = useRe.match(scanned.mid(matchStart));
        if (match.hasMatch()) {
            QString path = match.captured(2);
            items.m_usePaths.append(path);
        }
    }

    return items;
}
