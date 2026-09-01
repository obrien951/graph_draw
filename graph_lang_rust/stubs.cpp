#include "rustlex.h"
#include "rust_file_items.h"

#include <stdexcept>

namespace {

// Rust identifiers are ASCII alphanumeric plus '_'; used to tell a literal
// prefix (b, br, r) apart from the same letter inside a longer identifier.
bool rustIdentChar(const QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
        || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
        || c == QLatin1Char('_');
}

} // namespace

QString rustlex::blank(const QString& source)
{
    const int n = source.size();
    QString out;
    out.reserve(n);

    // Blank [from, to) with spaces, but keep every '\n' at its original
    // offset: callers index the raw source with offsets found in the blanked
    // text, so the layout invariant from sourcetext.h must hold here too.
    auto blanked = [&](int from, int to) {
        for (int j = from; j < to; ++j)
            out += (source.at(j) == QLatin1Char('\n')) ? source.at(j) : QLatin1Char(' ');
    };

    // Scan a quote-opened literal starting at `open`, honouring backslash
    // escapes. Returns the index just past the closing quote, or n when the
    // literal runs off the end of the input.
    auto scanQuoted = [&](int open, QChar quote) {
        int j = open + 1;
        while (j < n) {
            const QChar c = source.at(j);
            if (c == QLatin1Char('\\')) {
                j += 2;
                continue;
            }
            if (c == quote)
                return j + 1;
            ++j;
        }
        return n;
    };

    // Scan a raw string body whose opening quote sits at `openQuote` and whose
    // closer is the same run of hashes followed by a quote (r"…", r#"…"#,
    // r##"…"##). Returns the index just past the closer, or n when the run of
    // hashes never closes.
    auto scanRaw = [&](int openQuote, int hashes) {
        // The opener is r + hashes + quote, so the closer is the mirror:
        // quote + the same run of hashes (r#"…"#). The first such run wins,
        // which is exactly Rust's rule — the body cannot contain its closer.
        const QString closer = QLatin1Char('"') + QString(hashes, QLatin1Char('#'));
        const int end = source.indexOf(closer, openQuote + 1);
        return (end < 0) ? n : end + closer.size();
    };

    int i = 0;
    while (i < n) {
        const QChar c    = source.at(i);
        const QChar next = (i + 1 < n) ? source.at(i + 1) : QChar();
        const bool prefix = (i == 0) || !rustIdentChar(source.at(i - 1));

        int to = -1; // -1: no token recognised, emit the character verbatim

        if (c == QLatin1Char('/') && next == QLatin1Char('/')) {
            int j = i;
            while (j < n && source.at(j) != QLatin1Char('\n'))
                ++j;
            to = j; // the newline (if any) is left in place for the next pass
        } else if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
            // Rust block comments nest; a depth counter is the whole trick.
            int depth = 1;
            int j = i + 2;
            while (j < n && depth > 0) {
                const QChar cj = source.at(j);
                const QChar nj = (j + 1 < n) ? source.at(j + 1) : QChar();
                if (cj == QLatin1Char('/') && nj == QLatin1Char('*')) {
                    ++depth;
                    j += 2;
                } else if (cj == QLatin1Char('*') && nj == QLatin1Char('/')) {
                    --depth;
                    j += 2;
                } else {
                    ++j;
                }
            }
            to = j;
        } else if (c == QLatin1Char('"')) {
            to = scanQuoted(i, QLatin1Char('"'));
        } else if (prefix && c == QLatin1Char('r')) {
            // Raw string: r", r#"#, r##"## … The quote is what makes it a
            // string. r#type / r#match are raw *identifiers*: if the hash run
            // is not followed by a quote, emit the 'r' and let the identifier
            // scan on, or the naive match swallows the rest of the file.
            int j = i + 1;
            int hashes = 0;
            while (j < n && source.at(j) == QLatin1Char('#')) {
                ++hashes;
                ++j;
            }
            if (j < n && source.at(j) == QLatin1Char('"'))
                to = scanRaw(j, hashes);
        } else if (prefix && c == QLatin1Char('b')) {
            // Byte forms: b"…", b'…', br#* "…#* — and only when the 'b' is a
            // prefix of its own, not the tail of a longer identifier.
            if (next == QLatin1Char('"') || next == QLatin1Char('\'')) {
                to = scanQuoted(i + 1, next);
            } else if (next == QLatin1Char('r')) {
                int j = i + 2;
                int hashes = 0;
                while (j < n && source.at(j) == QLatin1Char('#')) {
                    ++hashes;
                    ++j;
                }
                if (j < n && source.at(j) == QLatin1Char('"'))
                    to = scanRaw(j, hashes);
            }
        } else if (c == QLatin1Char('\'')) {
            // Lifetime vs char. 'a / 'static / 'outer: are lifetimes and loop
            // labels and stay verbatim. A char literal is recognised by
            // lookahead: an escape, a closing quote one character on, or a
            // surrogate pair (emoji live outside the BMP and span two QChars).
            const QChar n1 = (i + 1 < n) ? source.at(i + 1) : QChar();
            const QChar n2 = (i + 2 < n) ? source.at(i + 2) : QChar();
            const bool charLiteral =
                n1 == QLatin1Char('\\')
                || n2 == QLatin1Char('\'')
                || (n1.unicode() >= 0xD800u && n1.unicode() <= 0xDBFFu
                    && n2.unicode() >= 0xDC00u && n2.unicode() <= 0xDFFFu);
            if (charLiteral)
                to = scanQuoted(i, QLatin1Char('\''));
        }

        if (to >= 0) {
            blanked(i, to);
            i = to;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

QString rustlex::leadingDoc(const QStringList& lines, int itemLine)
{
    // Walk up from the item, skipping blank lines and #[attribute] lines —
    // the dominant shape is a /// line, then #[derive(...)], then the item.
    // Collect the doc lines in reverse and stop at the first other line.
    QStringList collected;
    for (int i = itemLine - 1; i >= 0; --i) {
        const QString t = lines.at(i).trimmed();
        if (t.isEmpty() || t.startsWith(QLatin1String("#[")))
            continue;
        // A doc line starts with /// (outer) or //! (inner). In Rust a
        // fourth '/' or '!' is just doc content, not a plain comment, so
        // only the prefix decides.
        if (!t.startsWith(QLatin1String("///"))
            && !t.startsWith(QLatin1String("//!")))
            break;
        // Strip three characters, not two — stripping two leaves a stray
        // slash at the front of every doc line.
        collected.prepend(t.mid(3).trimmed());
    }
    while (!collected.isEmpty() && collected.first().isEmpty())
        collected.removeFirst();
    return collected.join(QLatin1Char(' ')).simplified();
}

RustFileItems RustFileItems::empty() {
    return RustFileItems{};
}

RustFileItems scanRustFile(const QString& source) {
    // NEW (B7). Single brace-depth scanner over blanked text.
    // Items are recognised only at item depth, so fns inside function bodies
    // and impls inside macro_rules! bodies never reach the graph.
    // impl headers need a real scan rather than a regex: find the first '{'
    // not nested in <>, (), or [], strip leading generics, split on a depth-0
    // ' for ', strip a trailing where clause.
    
    const QString blanked = rustlex::blank(source);
    const QStringList lines = blanked.split(QLatin1Char('\n'));
    
    RustFileItems items;
    int braceDepth = 0;
    int itemStart = -1;
    QString currentType;
    QString currentFunction;
    QString currentImpl;
    int currentLine = 0;
    
    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines[i];
        const QString trimmed = line.trimmed();
        
        // Count braces to determine depth
        for (int j = 0; j < line.length(); ++j) {
            const QChar c = line[j];
            if (c == QLatin1Char('{')) {
                if (braceDepth == 0) {
                    itemStart = i;
                }
                ++braceDepth;
            } else if (c == QLatin1Char('}')) {
                --braceDepth;
                if (braceDepth == 0 && itemStart >= 0) {
                    // End of item at item depth
                    // Process the item
                    const QString itemText = source.mid(itemStart, i - itemStart + 1);
                    if (trimmed.startsWith(QLatin1String("fn ")) && !trimmed.startsWith(QLatin1String("fn!"))) {
                        // Function item
                        RustFileItems::Item item;
                        item.label = trimmed.mid(3).trimmed(); // Skip "fn "
                        item.line = itemStart + 1;
                        items.functions.append(item);
                    } else if (trimmed.startsWith(QLatin1String("struct ")) ||
                               trimmed.startsWith(QLatin1String("enum ")) ||
                               trimmed.startsWith(QLatin1String("trait ")) ||
                               trimmed.startsWith(QLatin1String("union "))) {
                        // Type item
                        RustFileItems::Type type;
                        type.label = trimmed.split(QLatin1Char(' '))[1]; // Skip first word (type keyword)
                        type.line = itemStart + 1;
                        items.types.append(type);
                    } else if (trimmed.startsWith(QLatin1String("impl "))) {
                        // Impl item
                        // For impl headers, we need to parse the generic part and where clause
                        // Find the first '{' not nested in <>, (), or []
                        int bracePos = -1;
                        int parenDepth = 0;
                        int bracketDepth = 0;
                        int angleDepth = 0;
                        
                        for (int k = 0; k < trimmed.length(); ++k) {
                            const QChar c = trimmed[k];
                            if (c == QLatin1Char('<')) {
                                ++angleDepth;
                            } else if (c == QLatin1Char('>')) {
                                --angleDepth;
                            } else if (c == QLatin1Char('(')) {
                                ++parenDepth;
                            } else if (c == QLatin1Char(')')) {
                                --parenDepth;
                            } else if (c == QLatin1Char('[')) {
                                ++bracketDepth;
                            } else if (c == QLatin1Char(']')) {
                                --bracketDepth;
                            } else if (c == QLatin1Char('{') && angleDepth == 0 && parenDepth == 0 && bracketDepth == 0) {
                                bracePos = k;
                                break;
                            }
                        }
                        
                        if (bracePos >= 0) {
                            // Extract the impl header, strip generics and where clause
                            QString implHeader = trimmed.left(bracePos);
                            // Remove leading generics (between < and >) 
                            int openAngle = implHeader.indexOf(QLatin1Char('<'));
                            int closeAngle = -1;
                            if (openAngle >= 0) {
                                int angleDepth = 1;
                                for (int k = openAngle + 1; k < implHeader.length(); ++k) {
                                    if (implHeader[k] == QLatin1Char('<')) {
                                        ++angleDepth;
                                    } else if (implHeader[k] == QLatin1Char('>')) {
                                        --angleDepth;
                                        if (angleDepth == 0) {
                                            closeAngle = k;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (closeAngle >= 0) {
                                implHeader = implHeader.mid(0, openAngle) + implHeader.mid(closeAngle + 1);
                            }
                            // Remove trailing where clause if present
                            int wherePos = implHeader.indexOf(QLatin1String(" where "));
                            if (wherePos >= 0) {
                                implHeader = implHeader.left(wherePos);
                            }
                            
                            // The impl header now has the stripped content
                            RustFileItems::Type type;
                            type.label = implHeader.trimmed();
                            type.line = itemStart + 1;
                            items.types.append(type);
                        }
                    }
                    itemStart = -1;
                }
            }
        }
        
        currentLine = i;
    }
    
    // Process any remaining items at the end of file
    return items;
}
