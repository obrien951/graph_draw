#include "rustlex.h"
#include "rust_file_items.h"

#include <algorithm>
#include <stdexcept>
#include <QHash>
#include <QVector>

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

namespace {

// What classifyHeader() decided about the text immediately preceding a '{'
// or ';' — never persisted, just enough for the frame it produced to decide
// how its own closing event should be handled.
enum class ItemKind { TypeDecl, Trait, Impl, Function, Other };

struct Header {
    ItemKind kind = ItemKind::Other;
    QString  name;        // struct/enum/union/trait/fn name, or an impl's target type
    QString  traitName;   // non-empty only for "impl Trait for Type"
};

// One open brace on the stack. `header` is resolved at PUSH time, when the
// text between the previous delimiter and this '{' is still intact — the
// previous scanner read it off the CLOSING line instead, which is simply
// wrong for any multi-line item (i.e. almost all real Rust code).
struct Frame {
    Header header;
    int    line = 0;           // line the item's header started on
    int    openedAtDepth = 0;  // stack size at the moment this frame was pushed
    QVector<RustFileItems::Item> pendingMethods;
};

// Strips modifiers that can precede the keyword that actually names an
// item's kind, so "pub(crate) async unsafe fn f" and "fn f" classify the
// same way.
QString stripModifiers(QString header)
{
    header = header.trimmed();
    bool changed = true;
    while (changed) {
        changed = false;
        // One or more stacked `#[...]` attributes ahead of the item itself
        // ("#[derive(Debug)] pub struct User" when a doc comment above it
        // blanked to nothing but the attribute line stayed intact).
        if (header.startsWith(QLatin1String("#["))) {
            int depth = 0, k = 0;
            for (; k < header.size(); ++k) {
                if (header.at(k) == QLatin1Char('[')) ++depth;
                else if (header.at(k) == QLatin1Char(']')) { --depth; if (depth == 0) { ++k; break; } }
            }
            header = header.mid(k).trimmed();
            changed = true;
            continue;
        }
        if (header.startsWith(QLatin1String("pub(in "))) {
            const int close = header.indexOf(QLatin1Char(')'));
            if (close >= 0) { header = header.mid(close + 1).trimmed(); changed = true; continue; }
        }
        if (header.startsWith(QLatin1String("extern \""))) {
            const int close = header.indexOf(QLatin1Char('"'), 8);
            if (close >= 0) { header = header.mid(close + 1).trimmed(); changed = true; continue; }
        }
        static const QStringList prefixes = {
            QStringLiteral("pub(crate)"), QStringLiteral("pub(super)"),
            QStringLiteral("pub"), QStringLiteral("async"),
            QStringLiteral("unsafe"), QStringLiteral("const"),
            QStringLiteral("default"),
        };
        for (const QString& p : prefixes) {
            if (header.startsWith(p) && header.size() > p.size()
                && header.at(p.size()).isSpace()) {
                header = header.mid(p.size()).trimmed();
                changed = true;
                break;
            }
        }
    }
    return header;
}

// How many leading characters of (non-simplified) header text are noise
// before the token that actually names the item: whitespace, stacked
// `#[...]` attributes, and modifier keywords. A doc comment above an item
// blanks to spaces but an attribute line does not, so for something like
//     /// A user account.
//     #[derive(Debug, Serialize)]
//     pub struct User {
// the header text handed to classifyHeader() still carries the attribute,
// and the item's real line is wherever this offset lands, not stmtStart's.
int skipHeaderNoise(const QString& text)
{
    int i = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        while (i < text.size() && text.at(i).isSpace()) ++i;

        if (text.mid(i, 2) == QLatin1String("#[")) {
            int depth = 0, k = i;
            for (; k < text.size(); ++k) {
                if (text.at(k) == QLatin1Char('[')) ++depth;
                else if (text.at(k) == QLatin1Char(']')) { --depth; if (depth == 0) { ++k; break; } }
            }
            i = k;
            changed = true;
            continue;
        }
        if (text.mid(i, 7) == QLatin1String("pub(in ")) {
            const int close = text.indexOf(QLatin1Char(')'), i);
            if (close >= 0) { i = close + 1; changed = true; continue; }
        }
        if (text.mid(i, 8) == QLatin1String("extern \"")) {
            const int close = text.indexOf(QLatin1Char('"'), i + 8);
            if (close >= 0) { i = close + 1; changed = true; continue; }
        }
        static const QStringList prefixes = {
            QStringLiteral("pub(crate)"), QStringLiteral("pub(super)"),
            QStringLiteral("pub"), QStringLiteral("async"),
            QStringLiteral("unsafe"), QStringLiteral("const"),
            QStringLiteral("default"),
        };
        for (const QString& p : prefixes) {
            if (text.mid(i, p.size()) == p
                && i + p.size() < text.size() && text.at(i + p.size()).isSpace()) {
                i += p.size();
                changed = true;
                break;
            }
        }
    }
    while (i < text.size() && text.at(i).isSpace()) ++i;
    return i;
}

// The leading identifier (letters/digits/'_') in text, skipping any
// non-identifier characters first. Empty if none — e.g. an empty tuple
// struct's generic list with nothing before it.
QString firstIdentifier(const QString& text)
{
    int i = 0;
    while (i < text.size() && !(text.at(i).isLetter() || text.at(i) == QLatin1Char('_')))
        ++i;
    int j = i;
    while (j < text.size() && (text.at(j).isLetterOrNumber() || text.at(j) == QLatin1Char('_')))
        ++j;
    return text.mid(i, j - i);
}

// First occurrence of needle in text at angle-bracket depth 0 — used to
// split an impl header on " for " without being fooled by a generic bound
// that itself contains the word, like a higher-ranked trait bound.
int findTopLevel(const QString& text, const QString& needle)
{
    int angle = 0;
    for (int i = 0; i + needle.size() <= text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('<'))      ++angle;
        else if (c == QLatin1Char('>')) angle = std::max(0, angle - 1);
        else if (angle == 0 && text.mid(i, needle.size()) == needle)
            return i;
    }
    return -1;
}

// Classifies the text immediately before a '{' or ';' as a struct/enum/
// union/trait/fn/impl header (or Other, for anything else that happens to
// own a brace — an if/for/while/match block, a mod block, a macro body).
Header classifyHeader(QString text)
{
    text = stripModifiers(text);
    Header h;

    if (text.startsWith(QLatin1String("fn "))) {
        h.kind = ItemKind::Function;
        h.name = firstIdentifier(text.mid(3));
    } else if (text.startsWith(QLatin1String("struct ")) ||
               text.startsWith(QLatin1String("enum ")) ||
               text.startsWith(QLatin1String("union "))) {
        h.kind = ItemKind::TypeDecl;
        h.name = firstIdentifier(text.section(QLatin1Char(' '), 1));
    } else if (text.startsWith(QLatin1String("trait "))) {
        h.kind = ItemKind::Trait;
        h.name = firstIdentifier(text.mid(6));
    } else if (text.startsWith(QLatin1String("impl ")) || text.startsWith(QLatin1String("impl<"))) {
        h.kind = ItemKind::Impl;
        QString rest = text.mid(4).trimmed();
        // The impl's own generic parameter list ("impl<T>...") sits before
        // both the trait and the type and must not be mistaken for either —
        // strip it before looking for " for ".
        if (rest.startsWith(QLatin1Char('<'))) {
            int depth = 0, j = 0;
            for (; j < rest.size(); ++j) {
                if (rest.at(j) == QLatin1Char('<')) ++depth;
                else if (rest.at(j) == QLatin1Char('>')) { --depth; if (depth == 0) { ++j; break; } }
            }
            rest = rest.mid(j).trimmed();
        }
        const int forPos = findTopLevel(rest, QStringLiteral(" for "));
        QString typePart  = forPos >= 0 ? rest.mid(forPos + 5) : rest;
        QString traitPart = forPos >= 0 ? rest.left(forPos)    : QString();
        const int wherePos = findTopLevel(typePart, QStringLiteral(" where "));
        if (wherePos >= 0) typePart = typePart.left(wherePos);
        h.name = firstIdentifier(typePart);
        if (!traitPart.isEmpty()) {
            const int sep = traitPart.lastIndexOf(QLatin1String("::"));
            h.traitName = firstIdentifier(sep >= 0 ? traitPart.mid(sep + 2) : traitPart);
        }
    }
    return h;
}

// Classifies raw (non-simplified) header text and resolves the real source
// line it starts on — `baseLine` is the line `rawText` itself begins on.
struct Classified { Header header; int line; };

Classified classifyAt(const QString& rawText, int baseLine)
{
    const int skip = skipHeaderNoise(rawText);
    Classified c;
    c.line = baseLine + rawText.left(skip).count(QLatin1Char('\n'));
    c.header = classifyHeader(rawText.mid(skip).simplified());
    return c;
}

// Recursively expands one `use` path segment into leaf type names, handling
// `{a, b as c, d::{e, f}}` groups one level deep (as deep as real-world Rust
// nests them in practice) and dropping "as alias" — the pre-alias path is
// what actually names a repo-defined type.
void appendUsePathSegment(QString seg, QStringList* out)
{
    seg = seg.trimmed();
    if (seg.isEmpty()) return;

    const int brace = seg.indexOf(QLatin1Char('{'));
    if (brace >= 0 && seg.endsWith(QLatin1Char('}'))) {
        const QString prefix = seg.left(brace);
        const QString inner  = seg.mid(brace + 1, seg.length() - brace - 2);
        int depth = 0, start = 0;
        for (int i = 0; i <= inner.size(); ++i) {
            const bool atEnd = i == inner.size();
            const QChar c = atEnd ? QChar() : inner.at(i);
            if (!atEnd && c == QLatin1Char('{')) ++depth;
            else if (!atEnd && c == QLatin1Char('}')) --depth;
            if (atEnd || (c == QLatin1Char(',') && depth == 0)) {
                const QString member = inner.mid(start, i - start).trimmed();
                start = i + 1;
                if (!member.isEmpty())
                    appendUsePathSegment(prefix + member, out);
            }
        }
        return;
    }
    if (seg.endsWith(QLatin1String("::*"))) return; // glob: no specific leaf to resolve

    const int asPos = seg.indexOf(QLatin1String(" as "));
    if (asPos >= 0) seg = seg.left(asPos).trimmed();

    const int sep = seg.lastIndexOf(QLatin1String("::"));
    QString leaf = sep >= 0 ? seg.mid(sep + 2) : seg;
    if (leaf == QLatin1String("self")) {
        // `use a::b::self` (from an expanded `{self, ...}` group) refers to
        // module `b` itself, not a further leaf under it.
        const QString withoutSelf = sep >= 0 ? seg.left(sep) : QString();
        const int outerSep = withoutSelf.lastIndexOf(QLatin1String("::"));
        leaf = outerSep >= 0 ? withoutSelf.mid(outerSep + 2) : withoutSelf;
    }
    if (!leaf.isEmpty())
        out->append(leaf);
}

void appendUsePaths(const QString& useStmt, QStringList* out)
{
    appendUsePathSegment(useStmt.mid(4), out); // drop the leading "use "
}

// Leading //! lines at the very top of the file, Rust's convention for
// module-level documentation. Stops at the first line that isn't blank and
// isn't a //! line.
QString moduleDoc(const QString& source)
{
    QStringList collected;
    for (const QString& rawLine : source.split(QLatin1Char('\n'))) {
        const QString t = rawLine.trimmed();
        if (t.isEmpty()) continue;
        if (!t.startsWith(QLatin1String("//!"))) break;
        collected.append(t.mid(3).trimmed());
    }
    return collected.join(QLatin1Char(' ')).simplified();
}

} // namespace

RustFileItems scanRustFile(const QString& source) {
    // A single brace-depth scanner over blanked text (comments/strings
    // already neutralised so a brace inside one can't desync the depth).
    // Each '{' is classified once, at PUSH time, from the header text since
    // the previous ';'/'{'/'}' — the header belongs to the item being
    // opened, not to whatever line its closing brace happens to land on.
    //
    // A frame becomes a graph item only when it sat at file depth (a
    // top-level struct/enum/union/trait/impl/fn) or exactly one level
    // inside an already-recognised impl/trait (a method) — anything deeper,
    // including a fn nested inside another fn's body or a block expression,
    // is invisible by construction, not by special-casing.
    const QString blanked = rustlex::blank(source);
    const int n = blanked.size();

    RustFileItems items;
    QHash<QString, int> typeIndexByLabel;
    auto typeIndexFor = [&](const QString& label) -> int {
        auto it = typeIndexByLabel.constFind(label);
        if (it != typeIndexByLabel.constEnd()) return it.value();
        RustFileItems::Type t;
        t.label = label;
        items.types.append(t);
        const int idx = items.types.size() - 1;
        typeIndexByLabel.insert(label, idx);
        return idx;
    };

    QVector<Frame> stack;
    int stmtStart = 0;
    int itemLine = 0;    // line stmtStart currently sits on
    int currentLine = 0; // running line count as the scan proceeds

    for (int i = 0; i < n; ++i) {
        const QChar c = blanked.at(i);
        if (c == QLatin1Char('\n')) {
            ++currentLine;
            continue;
        }

        if (c == QLatin1Char('{')) {
            if (stack.isEmpty()
                && [&] {
                       const QString pending = blanked.mid(stmtStart, i - stmtStart);
                       return pending.mid(skipHeaderNoise(pending)).simplified()
                           .startsWith(QLatin1String("use "));
                   }()) {
                // A `use a::{b, c}` group brace is not an item scope — skip
                // to its matching close (nested groups included) and keep
                // accumulating the same statement, so the eventual ';'
                // handler sees the whole "use ...;" intact.
                int depth = 1;
                int j = i + 1;
                for (; j < n && depth > 0; ++j) {
                    if (blanked.at(j) == QLatin1Char('\n')) ++currentLine;
                    else if (blanked.at(j) == QLatin1Char('{')) ++depth;
                    else if (blanked.at(j) == QLatin1Char('}')) --depth;
                }
                i = j - 1;
                continue;
            }
            const Classified c = classifyAt(blanked.mid(stmtStart, i - stmtStart), itemLine);
            Frame f;
            f.header = c.header;
            f.line = c.line;
            f.openedAtDepth = stack.size();
            stack.append(f);
            stmtStart = i + 1;
            itemLine = currentLine;
            continue;
        }

        if (c == QLatin1Char('}')) {
            if (stack.isEmpty()) { stmtStart = i + 1; itemLine = currentLine; continue; }
            const Frame closed = stack.takeLast();
            const bool topLevel = closed.openedAtDepth == 0;
            const bool parentIsImplOrTrait = !stack.isEmpty()
                && (stack.last().header.kind == ItemKind::Impl
                    || stack.last().header.kind == ItemKind::Trait);

            if (topLevel) {
                switch (closed.header.kind) {
                case ItemKind::Function:
                    if (!closed.header.name.isEmpty())
                        items.functions.append(RustFileItems::Item{closed.header.name, closed.line});
                    break;
                case ItemKind::TypeDecl:
                case ItemKind::Trait: {
                    if (closed.header.name.isEmpty()) break;
                    const int idx = typeIndexFor(closed.header.name);
                    items.types[idx].line = closed.line;
                    for (const auto& m : closed.pendingMethods)
                        items.types[idx].methods.append(m);
                    break;
                }
                case ItemKind::Impl: {
                    if (closed.header.name.isEmpty()) break;
                    const int idx = typeIndexFor(closed.header.name);
                    if (!closed.header.traitName.isEmpty()
                        && !items.types[idx].implementsTraits.contains(closed.header.traitName))
                        items.types[idx].implementsTraits.append(closed.header.traitName);
                    for (const auto& m : closed.pendingMethods)
                        items.types[idx].methods.append(m);
                    break;
                }
                default: break;
                }
            } else if (closed.openedAtDepth == 1 && parentIsImplOrTrait
                       && closed.header.kind == ItemKind::Function && !closed.header.name.isEmpty()) {
                stack.last().pendingMethods.append(RustFileItems::Item{closed.header.name, closed.line});
            }
            // Depth >= 2, or depth-1 inside anything that isn't impl/trait
            // (a loop body, a match arm, a plain fn's own nested fn): not a
            // graph item.
            stmtStart = i + 1;
            itemLine = currentLine;
            continue;
        }

        if (c == QLatin1Char(';')) {
            const QString rawStmt = blanked.mid(stmtStart, i - stmtStart);
            const int stmtLine = itemLine;
            stmtStart = i + 1;
            itemLine = currentLine;

            if (stack.isEmpty()) {
                // Tuple/unit struct|enum|union declarations have no braces
                // at all ("struct Point(i32, i32);", "struct Unit;"), and
                // `use` declarations (possibly "pub use" re-exports) live
                // here too.
                const int skip = skipHeaderNoise(rawStmt);
                const QString noiseFree = rawStmt.mid(skip).simplified();
                if (noiseFree.startsWith(QLatin1String("use ")))
                    appendUsePaths(noiseFree, &items.usePaths);
                else {
                    const Classified c2 = classifyAt(rawStmt, stmtLine);
                    if (c2.header.kind == ItemKind::TypeDecl && !c2.header.name.isEmpty())
                        items.types[typeIndexFor(c2.header.name)].line = c2.line;
                }
            } else if (stack.last().header.kind == ItemKind::Trait) {
                // A required trait method has no body: "fn foo(&self);".
                const Classified c2 = classifyAt(rawStmt, stmtLine);
                if (c2.header.kind == ItemKind::Function && !c2.header.name.isEmpty())
                    stack.last().pendingMethods.append(RustFileItems::Item{c2.header.name, c2.line});
            }
        }
    }

    items.innerDoc = moduleDoc(source);
    return items;
}
