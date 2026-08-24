#include "blank.h"
#include <QRegularExpression>

QString blank(const QString& text)
{
    QString out;
    out.reserve(text.size());

    enum State { Code, LineComment, BlockComment, RawString, RawStringHash } st = Code;
    int depth = 0;  // for nested block comments
    int rawDepth = 0;  // for nested raw strings

    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        const QChar n = (i + 1 < text.size()) ? text[i + 1] : QChar();

        switch (st) {
        case Code:
            if (c == '/' && n == '/') {
                st = LineComment;
                out += ' ';
            } else if (c == '/' && n == '*') {
                st = BlockComment;
                out += ' ';
            } else if (c == 'r' && (i + 1 < text.size()) && text[i + 1] == '"') {
                // Raw string: r"..." or r#"...# or r##"..."##
                st = RawString;
                out += ' ';
            } else if (c == 'r' && (i + 1 < text.size()) && text[i + 1] == '#') {
                // Raw string with hash: r#"...# or r##"..."##
                st = RawStringHash;
                out += ' ';
            } else {
                out += c;
            }
            break;

        case LineComment:
            if (c == '\n') {
                st = Code;
                out += '\n';
            } else {
                out += ' ';
            }
            break;

        case BlockComment:
            if (c == '*' && n == '/') {
                st = Code;
                out += "  ";  // preserve two spaces for alignment
                ++i;  // skip the '/'
            } else {
                out += ' ';
            }
            break;

        case RawString:
            if (c == '"') {
                st = Code;
                out += ' ';
            } else {
                out += ' ';
            }
            break;

        case RawStringHash:
            if (c == '#') {
                // Check if this closes the raw string
                // Count opening hashes
                int openHashes = 0;
                for (int j = 0; j < i; ++j) {
                    if (text[j] == 'r' && j + 1 < text.size() && text[j + 1] == '#') {
                        openHashes++;
                    }
                }
                // Count closing hashes (simplified: just check if we've seen enough)
                // For simplicity, we treat r#...# as depth 1
                if (rawDepth > 0) {
                    rawDepth--;
                    out += ' ';
                } else {
                    st = Code;
                    out += ' ';
                }
            } else {
                out += ' ';
            }
            break;
        }
    }

    return out;
}
