#include <catch2/catch_test_macros.hpp>

#include "rustlex.h"

#include <QString>
#include <QStringList>

// Unit tests for the Rust-dialect blanker, rustlex::blank. It must uphold the
// same layout invariant as sourcetext::blankC — same length, every '\n' at its
// original offset — while additionally understanding the Rust-specific token
// shapes the C++ blanker gets wrong: nesting block comments, raw strings with
// hash delimiters, byte literals, raw identifiers, and the lifetime-vs-char
// ambiguity of the apostrophe.
//
// No QApplication is created or needed — graph_lang_rust links Qt Core only
// for these helpers.

using rustlex::blank;
using rustlex::leadingDoc;

namespace {

void requireLayoutPreserved(const QString& src)
{
    const QString blanked = blank(src);
    REQUIRE(blanked.size() == src.size());
    for (int i = 0; i < src.size(); ++i) {
        INFO("offset " << i);
        REQUIRE((src.at(i) == QLatin1Char('\n')) == (blanked.at(i) == QLatin1Char('\n')));
    }
}

QString spaces(int count)
{
    return QString(count, QLatin1Char(' '));
}

} // namespace

TEST_CASE("blank preserves length and newline offsets for a Rust corpus",
          "[rustlex][blank]")
{
    QString emoji;
    emoji.append(QChar(0xD83D));
    emoji.append(QChar(0xDE00));
    const QString surrogateChar = QStringLiteral("let e = '") + emoji
                                + QStringLiteral("';\n");
    const QStringList corpus = {
        QStringLiteral(""),
        QStringLiteral("\n\n\n"),
        QStringLiteral("fn main() { let x = 1; }\n"),
        // line comments, including one that runs to end of input
        QStringLiteral("let a = 1; // trailing\nlet b = 2;\n"),
        QStringLiteral("let a = 1; // no newline at eof"),
        // nested block comments
        QStringLiteral("/* outer /* inner */ still */ let x;\n"),
        QStringLiteral("/** doc */\n/// inner doc\n"),
        // regular string with escaped quote and escaped backslash
        QStringLiteral("let s = \"a\\\"b\\\\c\"; let y;\n"),
        // raw strings with 0, 1 and 2 hashes, braces inside
        QStringLiteral("let s = r\"raw { brace }\";\n"),
        QStringLiteral("let s = r#\"has \" quote\"#;\n"),
        QStringLiteral("let s = r##\"two ## one\"##;\n"),
        // byte forms
        QStringLiteral("let s = br#\"byte raw\"#; let b = b\"plain\";\n"),
        QStringLiteral("let c = b'{' ; let d = '\\n'; let e = '\\u{1F600}';\n"),
        // lifetimes everywhere, no char literals
        QStringLiteral("fn f<'a>(x: &'a str) -> &'a str { x }\n"),
        QStringLiteral("'outer: loop { break 'outer; }\n"),
        // elided lifetime and a raw identifier
        QStringLiteral("let x: &'_ str; let r#match = 1;\n"),
        // UNTERMINATED raw string running past a newline
        QStringLiteral("let bad = r\"never closed\nfn g() {}\n"),
        // UNTERMINATED block comment running to end of input
        QStringLiteral("/* never closed\nfn h() {}\n"),
        // surrogate-pair char literal (emoji spans two QChars)
        surrogateChar,
    };

    for (int i = 0; i < corpus.size(); ++i) {
        INFO("corpus entry " << i);
        requireLayoutPreserved(corpus.at(i));
    }
}

TEST_CASE("blank blanks a line comment but keeps surrounding code",
          "[rustlex][blank]")
{
    const QString src = QStringLiteral("let a = 1; // secret\nlet b = 2;\n");
    const QString out = blank(src);

    REQUIRE(out == QStringLiteral("let a = 1; ") + spaces(9)
                + QStringLiteral("\nlet b = 2;\n"));
    REQUIRE_FALSE(out.contains(QStringLiteral("secret")));
}

TEST_CASE("blank handles nested block comments with a depth counter",
          "[rustlex][blank]")
{
    SECTION("inner comment does not close the outer one") {
        const QString src = QStringLiteral("a /* o /* i */ m */ b");
        const QString out = blank(src);

        REQUIRE(out == QStringLiteral("a") + spaces(19) + QStringLiteral("b"));
        REQUIRE_FALSE(out.contains(QLatin1Char('o')));
        REQUIRE_FALSE(out.contains(QLatin1Char('m')));
    }

    SECTION("the whole /* ... */ including both markers is blanked") {
        const QString src = QStringLiteral("x /* c */ y");
        const QString out = blank(src);

        REQUIRE(out == QStringLiteral("x") + spaces(9) + QStringLiteral("y"));
        REQUIRE_FALSE(out.contains(QLatin1Char('c')));
    }

    SECTION("a /**/ doc-style comment is blanked whole") {
        const QString src = QStringLiteral("a /**/ b");
        const QString out = blank(src);

        REQUIRE(out == QStringLiteral("a") + spaces(6) + QStringLiteral("b"));
    }

    SECTION("newlines inside a multi-line comment survive at their offsets") {
        const QString src = QStringLiteral("x /* c1\nc2 */ y");
        const QString out = blank(src);

        REQUIRE(out.indexOf(QLatin1Char('\n')) == src.indexOf(QLatin1Char('\n')));
        REQUIRE(out.count(QLatin1Char('\n')) == 1);
        REQUIRE(out == QStringLiteral("x ") + spaces(5)
                    + QStringLiteral("\n") + spaces(5) + QStringLiteral(" y"));
    }
}

TEST_CASE("blank matches raw strings by hash count", "[rustlex][blank]")
{
    SECTION("zero hashes") {
        const QString src = QStringLiteral("let s = r\"{\";");
        REQUIRE(blank(src) == QStringLiteral("let s = ") + spaces(4)
                              + QStringLiteral(";"));
        REQUIRE(blank(src).count(QLatin1Char('{')) == 0);
    }

    SECTION("one hash — a quote inside the body is inert") {
        const QString src = QStringLiteral("let s = r#\"{\"#;");
        REQUIRE(blank(src) == QStringLiteral("let s = ") + spaces(6)
                              + QStringLiteral(";"));
        REQUIRE(blank(src).count(QLatin1Char('{')) == 0);
    }

    SECTION("two hashes — ## inside the body is not the closer") {
        const QString src = QStringLiteral("let s = r##\"two ## one{\"##;");
        REQUIRE(blank(src) == QStringLiteral("let s = ") + spaces(18)
                              + QStringLiteral(";"));
        REQUIRE(blank(src).count(QLatin1Char('{')) == 0);
    }
}

TEST_CASE("blank does not treat raw identifiers as raw strings",
          "[rustlex][blank]")
{
    SECTION("r#type and r#match pass through verbatim") {
        const QString src = QStringLiteral("let r#type = 1; let r#match = 2;");
        REQUIRE(blank(src) == src);
    }

    SECTION("the trap: an identifier does not swallow a later raw string") {
        // A naive r#…" match would start at r#match and swallow the file;
        // the code brace must survive, the raw string brace must not.
        const QString src = QStringLiteral("r#match { } r#\"{\"#");
        const QString out = blank(src);

        REQUIRE(out == QStringLiteral("r#match { } ") + spaces(6));
        REQUIRE(out.count(QLatin1Char('{')) == 1);
        REQUIRE(out.count(QLatin1Char('}')) == 1);
    }
}

TEST_CASE("blank handles byte forms", "[rustlex][blank]")
{
    SECTION("byte string") {
        const QString src = QStringLiteral("let b = b\"{\";");
        REQUIRE(blank(src) == QStringLiteral("let b = ") + spaces(4)
                              + QStringLiteral(";"));
        REQUIRE(blank(src).count(QLatin1Char('{')) == 0);
    }

    SECTION("raw byte string") {
        const QString src = QStringLiteral("let s = br#\"{\"#;");
        REQUIRE(blank(src) == QStringLiteral("let s = ") + spaces(7)
                              + QStringLiteral(";"));
        REQUIRE(blank(src).count(QLatin1Char('{')) == 0);
    }

    SECTION("byte char") {
        const QString src = QStringLiteral("let c = b'{';");
        REQUIRE(blank(src) == QStringLiteral("let c = ") + spaces(4)
                              + QStringLiteral(";"));
        REQUIRE(blank(src).count(QLatin1Char('{')) == 0);
    }

    SECTION("b inside a longer identifier is not a prefix") {
        const QString src = QStringLiteral("let bar = 1; let brx = 2;");
        REQUIRE(blank(src) == src);
    }
}

TEST_CASE("blank disambiguates lifetimes from char literals",
          "[rustlex][blank]")
{
    SECTION("lifetimes and labels stay verbatim") {
        const QString fnSig = QStringLiteral("fn f<'a>(x: &'a str) -> &'a str { x }");
        REQUIRE(blank(fnSig) == fnSig);

        const QString label = QStringLiteral("'outer: loop { break 'outer; }");
        REQUIRE(blank(label) == label);
        REQUIRE(blank(label).count(QLatin1Char('{')) == 1);
    }

    SECTION("elided lifetime '_ is not a char literal") {
        const QString src = QStringLiteral("let x: &'_ str;");
        REQUIRE(blank(src) == src);
    }

    SECTION("plain char literal is blanked") {
        const QString src = QStringLiteral("let a = 'x';");
        REQUIRE(blank(src) == QStringLiteral("let a = ") + spaces(3)
                              + QStringLiteral(";"));
    }

    SECTION("escaped char literal is blanked") {
        const QString src = QStringLiteral("let c = '\\n';");
        REQUIRE(blank(src) == QStringLiteral("let c = ") + spaces(4)
                              + QStringLiteral(";"));
    }

    SECTION("unicode escape char literal hides its braces") {
        const QString src = QStringLiteral("let d = '\\u{1F600}';");
        const QString out = blank(src);
        REQUIRE(out == QStringLiteral("let d = ") + spaces(11)
                          + QStringLiteral(";"));
        REQUIRE(out.count(QLatin1Char('{')) == 0);
    }

    SECTION("surrogate-pair char literal is blanked") {
        // Built from explicit surrogates: an emoji char is two QChars, which
        // is exactly the shape the lookahead must recognise.
        QString emoji;
        emoji.append(QChar(0xD83D));
        emoji.append(QChar(0xDE00));
        const QString src = QStringLiteral("let e = '") + emoji
                          + QStringLiteral("';");
        const QString out = blank(src);
        REQUIRE(out.size() == src.size());
        REQUIRE(out == QStringLiteral("let e = ") + spaces(4)
                          + QStringLiteral(";"));
    }
}

TEST_CASE("blank honours backslash escapes in regular strings",
          "[rustlex][blank]")
{
    const QString src = QStringLiteral("let s = \"a\\\"b\";");
    REQUIRE(blank(src) == QStringLiteral("let s = ") + spaces(6)
                              + QStringLiteral(";"));
    REQUIRE_FALSE(blank(src).contains(QLatin1Char('"')));
}

TEST_CASE("blank leaves an unterminated literal or comment blanked to the end",
          "[rustlex][blank]")
{
    SECTION("unterminated raw string") {
        const QString src = QStringLiteral("let bad = r\"oops\nfn g() {}");
        const QString out = blank(src);

        REQUIRE(out.startsWith(QStringLiteral("let bad = ")));
        REQUIRE_FALSE(out.contains(QStringLiteral("fn g() {}")));
        REQUIRE(out.count(QLatin1Char('\n')) == src.count(QLatin1Char('\n')));
    }

    SECTION("unterminated block comment") {
        const QString src = QStringLiteral("/* never closed\nfn h() {}");
        const QString out = blank(src);

        REQUIRE(out.count(QLatin1Char('\n')) == src.count(QLatin1Char('\n')));
        REQUIRE_FALSE(out.contains(QStringLiteral("fn h() {}")));
        REQUIRE_FALSE(out.contains(QLatin1Char('/')));
    }
}

// Unit tests for rustlex::leadingDoc: the doc comment sitting directly above
// an item. It must strip three characters from each /// and //! line (not two,
// which is what left a stray slash), and it must skip blank lines and
// #[attribute] lines between the doc and the item — the dominant real shape
// is a /// line, then #[derive(...)], then the item.

TEST_CASE("leadingDoc strips three characters from doc lines",
          "[rustlex][leadingDoc]")
{
    SECTION("a single /// line above the item") {
        const QStringList lines = {
            QStringLiteral("/// Counts up."),
            QStringLiteral("struct Counter;"),
        };
        REQUIRE(leadingDoc(lines, 1) == QStringLiteral("Counts up."));
    }

    SECTION("a //! module doc line strips three characters too") {
        const QStringList lines = {
            QStringLiteral("//! Module docs."),
            QStringLiteral("fn entry() {}"),
        };
        REQUIRE(leadingDoc(lines, 1) == QStringLiteral("Module docs."));
    }

    SECTION("no space after the slashes is fine") {
        const QStringList lines = {
            QStringLiteral("///tight"),
            QStringLiteral("struct Tight;"),
        };
        REQUIRE(leadingDoc(lines, 1) == QStringLiteral("tight"));
    }

    SECTION("multiple doc lines join in source order") {
        const QStringList lines = {
            QStringLiteral("/// Line one."),
            QStringLiteral("/// Line two."),
            QStringLiteral("struct Pair;"),
        };
        REQUIRE(leadingDoc(lines, 2)
                == QStringLiteral("Line one. Line two."));
    }

    SECTION("a bare /// line contributes nothing") {
        const QStringList lines = {
            QStringLiteral("///"),
            QStringLiteral("struct Bare;"),
        };
        REQUIRE(leadingDoc(lines, 1).isEmpty());
    }

    SECTION("a fourth slash or bang is doc content, still a doc line") {
        const QStringList lines = {
            QStringLiteral("//// path / note"),
            QStringLiteral("struct Slash;"),
        };
        REQUIRE(leadingDoc(lines, 1) == QStringLiteral("/ path / note"));

        const QStringList bang = {
            QStringLiteral("//!!"),
            QStringLiteral("struct Bang;"),
        };
        REQUIRE(leadingDoc(bang, 1) == QStringLiteral("!"));
    }
}

TEST_CASE("leadingDoc skips blank lines and attributes above the item",
          "[rustlex][leadingDoc]")
{
    SECTION("the dominant shape: doc, then derive, then item") {
        const QStringList lines = {
            QStringLiteral("/// A derived value."),
            QStringLiteral("#[derive(Debug, Clone)]"),
            QStringLiteral("struct Value;"),
        };
        REQUIRE(leadingDoc(lines, 2) == QStringLiteral("A derived value."));
    }

    SECTION("several attributes and blank lines are skipped") {
        const QStringList lines = {
            QStringLiteral("/// Documented."),
            QStringLiteral(""),
            QStringLiteral("#[cfg(unix)]"),
            QStringLiteral("#[derive(Default)]"),
            QStringLiteral("struct Item;"),
        };
        REQUIRE(leadingDoc(lines, 4) == QStringLiteral("Documented."));
    }

    SECTION("an attribute between doc lines does not end the doc") {
        const QStringList lines = {
            QStringLiteral("/// First part."),
            QStringLiteral("#[derive(PartialEq)]"),
            QStringLiteral("/// Second part."),
            QStringLiteral("struct Mixed;"),
        };
        REQUIRE(leadingDoc(lines, 3)
                == QStringLiteral("First part. Second part."));
    }

    SECTION("a blank line between the doc and the item is skipped") {
        const QStringList lines = {
            QStringLiteral("/// Spaced doc."),
            QStringLiteral(""),
            QStringLiteral("struct Spaced;"),
        };
        REQUIRE(leadingDoc(lines, 2) == QStringLiteral("Spaced doc."));
    }
}

TEST_CASE("leadingDoc stops at the first non-doc line",
          "[rustlex][leadingDoc]")
{
    SECTION("the previous item's code ends the walk") {
        const QStringList lines = {
            QStringLiteral("fn previous() {}"),
            QStringLiteral("/// Only for the struct."),
            QStringLiteral("struct Next;"),
        };
        REQUIRE(leadingDoc(lines, 2)
                == QStringLiteral("Only for the struct."));
    }

    SECTION("a plain // comment is not a doc line") {
        const QStringList lines = {
            QStringLiteral("// just a comment"),
            QStringLiteral("struct Plain;"),
        };
        REQUIRE(leadingDoc(lines, 1).isEmpty());
    }

    SECTION("an item at the top of the file has no leading doc") {
        const QStringList lines = {
            QStringLiteral("struct Top;"),
        };
        REQUIRE(leadingDoc(lines, 0).isEmpty());
    }

    SECTION("attributes alone, with no doc, yield nothing") {
        const QStringList lines = {
            QStringLiteral("#[derive(Debug)]"),
            QStringLiteral("struct Bare;"),
        };
        REQUIRE(leadingDoc(lines, 1).isEmpty());
    }
}
