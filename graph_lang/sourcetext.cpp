#include "sourcetext.h"

#include <QRegularExpression>

// Extracted verbatim from graph_analyze/repoanalyzer.cpp (B1 of
// IMPLEMENTATION_PLAN.md). The algorithms below are deliberately unchanged —
// tests/test_repoanalyzer.cpp is the regression contract for this extraction.
namespace sourcetext {

// Replace comments and string/char literals with spaces (newlines preserved) so
// declaration matching never trips over commented-out or quoted code, while line
// numbers stay aligned with the original text.
QString blankC(const QString& s)
{
    QString out;
    out.reserve(s.size());
    enum State { Code, Line, Block, Str, Chr } st = Code;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        const QChar n = (i + 1 < s.size()) ? s[i + 1] : QChar();
        auto keepNl = [&](QChar ch) { out += (ch == QLatin1Char('\n')) ? ch : QChar(' '); };
        switch (st) {
        case Code:
            if (c == '/' && n == '/') { st = Line;  out += ' '; }
            else if (c == '/' && n == '*') { st = Block; out += ' '; }
            else if (c == '"')  { st = Str; out += ' '; }
            else if (c == '\'') { st = Chr; out += ' '; }
            else out += c;
            break;
        case Line:
            if (c == '\n') { st = Code; out += '\n'; } else out += ' ';
            break;
        case Block:
            if (c == '*' && n == '/') { st = Code; out += "  "; ++i; } else keepNl(c);
            break;
        case Str:
            if (c == '\\') { out += ' '; if (i + 1 < s.size()) { keepNl(n); ++i; } }
            else if (c == '"') { st = Code; out += ' '; } else keepNl(c);
            break;
        case Chr:
            if (c == '\\') { out += ' '; if (i + 1 < s.size()) { keepNl(n); ++i; } }
            else if (c == '\'') { st = Code; out += ' '; } else keepNl(c);
            break;
        }
    }
    return out;
}

// Collect the contiguous comment block immediately above line declLine (0-based)
// in rawLines, stripped of comment markers. Empty if there is none.
QString leadingComment(const QStringList& rawLines, int declLine)
{
    QStringList collected;
    int i = declLine - 1;
    // Skip a single blank line between the comment and the declaration.
    while (i >= 0 && rawLines[i].trimmed().isEmpty() && collected.isEmpty())
        --i;
    for (; i >= 0; --i) {
        QString t = rawLines[i].trimmed();
        if (t.startsWith(QLatin1String("//"))) {
            t.remove(0, 2);
            collected.prepend(t.trimmed());
        } else if (t.endsWith(QLatin1String("*/")) || t.startsWith(QLatin1String("*"))
                   || t.startsWith(QLatin1String("/*"))) {
            t.remove(QRegularExpression(QStringLiteral("^/\\*+|\\*+/$|^\\*+")));
            collected.prepend(t.trimmed());
            if (rawLines[i].trimmed().startsWith(QLatin1String("/*")))
                break;
        } else {
            break;
        }
    }
    while (!collected.isEmpty() && collected.first().isEmpty())
        collected.removeFirst();
    return collected.join(QLatin1Char(' ')).simplified();
}

// Index just past the '}' matching the '{' at openBrace; text.size() when the
// brace is unmatched. This is the loop that used to sit inline in the class-body
// scanner, so the "one past" result and the unmatched fallback are both load
// bearing for callers doing text.mid(open + 1, matchingBrace(text, open) - open - 2).
int matchingBrace(const QString& text, int openBrace)
{
    if (openBrace < 0 || openBrace >= text.size() || text[openBrace] != QLatin1Char('{'))
        return text.size();
    int depth = 0, j = openBrace;
    for (; j < text.size(); ++j) {
        if (text[j] == '{') ++depth;
        else if (text[j] == '}') { if (--depth == 0) { ++j; break; } }
    }
    return j;
}

int lineOf(const QString& text, int offset)
{
    return text.left(offset).count(QLatin1Char('\n'));  // 0-based
}

} // namespace sourcetext
