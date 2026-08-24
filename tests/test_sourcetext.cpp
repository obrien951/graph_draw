#include <catch2/catch_test_macros.hpp>

#include "sourcetext.h"

#include <QString>
#include <QStringList>

// Unit tests for the language-neutral C-family text helpers extracted from
// graph_analyze/repoanalyzer.cpp. These pin the behaviour that actually exists
// today: tests/test_repoanalyzer.cpp is the regression contract for the
// extraction, so anything asserted here must stay true of the original code.
//
// No QApplication is created or needed — sourcetext links Qt Core only, and
// keeping these tests GUI-free is a deliberate property of the design.

using sourcetext::blankC;
using sourcetext::leadingComment;
using sourcetext::lineOf;
using sourcetext::matchingBrace;

namespace {

// The headline invariant: blanking never changes the length of the text and
// never moves a newline. Callers index the *raw* source with offsets found in
// the blanked source, so both halves are load-bearing.
void requireLayoutPreserved(const QString& src)
{
    const QString blanked = blankC(src);
    REQUIRE(blanked.size() == src.size());
    for (int i = 0; i < src.size(); ++i) {
        INFO("offset " << i);
        REQUIRE((src.at(i) == QLatin1Char('\n')) == (blanked.at(i) == QLatin1Char('\n')));
    }
}

} // namespace

TEST_CASE("blankC preserves length and newline offsets for varied input",
          "[sourcetext][blankC]")
{
    const QStringList corpus = {
        QStringLiteral(""),
        QStringLiteral("\n\n\n"),
        QStringLiteral("int plain = 1;\n"),
        // line comment, including one that runs to end of input without a newline
        QStringLiteral("int a = 1; // trailing\nint b;\n"),
        QStringLiteral("int a; // no newline at eof"),
        // block comment, single line and spanning lines
        QStringLiteral("int a; /* short */ int b;\n"),
        QStringLiteral("/* multi\n   line\n   comment */ int x;\n"),
        // string literals with escaped quote and escaped backslash
        QStringLiteral("const char* s = \"a\\\"b\\\\c\"; int y;\n"),
        // char literals, including an escape
        QStringLiteral("char c = '\\n'; char d = 'x';\n"),
        // a backslash as the very last character of the input
        QStringLiteral("const char* s = \"tail\\"),
        // UNTERMINATED string literal running past a newline
        QStringLiteral("const char* bad = \"unterminated\nint z;\n"),
        // UNTERMINATED block comment running to end of input
        QStringLiteral("int a;\n/* never closed\nint w;\n"),
        // braces and quotes tangled together
        QStringLiteral("class A { QString s = \"}\"; char c = '{'; };\n"),
    };

    for (int i = 0; i < corpus.size(); ++i) {
        INFO("corpus entry " << i);
        requireLayoutPreserved(corpus.at(i));
    }
}

TEST_CASE("blankC blanks a line comment but keeps surrounding code",
          "[sourcetext][blankC]")
{
    const QString src = QStringLiteral("int a = 1; // secret\nint b = 2;\n");
    const QString out = blankC(src);

    REQUIRE(out == QStringLiteral("int a = 1;          \nint b = 2;\n"));
    REQUIRE_FALSE(out.contains(QStringLiteral("secret")));
}

TEST_CASE("blankC blanks a block comment and keeps its newlines",
          "[sourcetext][blankC]")
{
    const QString src = QStringLiteral("int a; /* c1\nc2 */ int b;");
    const QString out = blankC(src);

    REQUIRE(out == QStringLiteral("int a;      \n      int b;"));
    // The newline that was inside the comment survives at its original offset.
    REQUIRE(out.indexOf(QLatin1Char('\n')) == src.indexOf(QLatin1Char('\n')));
    REQUIRE(out.count(QLatin1Char('\n')) == 1);
}

TEST_CASE("blankC blanks string and char literals", "[sourcetext][blankC]")
{
    SECTION("string literal") {
        const QString src = QStringLiteral("s = \"hello\"; t = 3;");
        REQUIRE(blankC(src) == QStringLiteral("s =        ; t = 3;"));
    }

    SECTION("char literal") {
        const QString src = QStringLiteral("c = 'x'; t = 3;");
        REQUIRE(blankC(src) == QStringLiteral("c =    ; t = 3;"));
    }

    SECTION("escaped quote does not end the string") {
        const QString src = QStringLiteral("s = \"a\\\"b\"; t = 3;");
        REQUIRE(blankC(src) == QStringLiteral("s =       ; t = 3;"));
        REQUIRE_FALSE(blankC(src).contains(QLatin1Char('"')));
    }

    SECTION("braces inside literals are hidden from brace matching") {
        const QString src = QStringLiteral("class A { QString s = \"}\"; };");
        const QString out = blankC(src);
        REQUIRE(out.count(QLatin1Char('{')) == 1);
        REQUIRE(out.count(QLatin1Char('}')) == 1);
    }
}

TEST_CASE("blankC leaves an unterminated literal or comment blanked to the end",
          "[sourcetext][blankC]")
{
    SECTION("unterminated string") {
        const QString src = QStringLiteral("const char* bad = \"oops\nint z;\n");
        const QString out = blankC(src);
        REQUIRE(out.startsWith(QStringLiteral("const char* bad = ")));
        REQUIRE_FALSE(out.contains(QStringLiteral("int z;")));
        REQUIRE(out.count(QLatin1Char('\n')) == src.count(QLatin1Char('\n')));
    }

    SECTION("unterminated block comment") {
        const QString src = QStringLiteral("int a;\n/* never closed\nint w;\n");
        const QString out = blankC(src);
        REQUIRE(out.startsWith(QStringLiteral("int a;\n")));
        REQUIRE_FALSE(out.contains(QStringLiteral("int w;")));
        REQUIRE(out.count(QLatin1Char('\n')) == src.count(QLatin1Char('\n')));
    }
}

TEST_CASE("leadingComment collects a // block above a declaration",
          "[sourcetext][leadingComment]")
{
    const QStringList lines = {
        QStringLiteral("// One"),
        QStringLiteral("// Two"),
        QStringLiteral("class A {};"),
    };
    REQUIRE(leadingComment(lines, 2) == QStringLiteral("One Two"));
}

TEST_CASE("leadingComment strips block-comment markers",
          "[sourcetext][leadingComment]")
{
    SECTION("multi-line /** ... */") {
        const QStringList lines = {
            QStringLiteral("/** Doc line one"),
            QStringLiteral(" * more"),
            QStringLiteral(" */"),
            QStringLiteral("class A {};"),
        };
        REQUIRE(leadingComment(lines, 3) == QStringLiteral("Doc line one more"));
    }

    SECTION("single-line /* ... */") {
        const QStringList lines = {
            QStringLiteral("/* Foo bar */"),
            QStringLiteral("class A {};"),
        };
        REQUIRE(leadingComment(lines, 1) == QStringLiteral("Foo bar"));
    }
}

TEST_CASE("leadingComment tolerates a blank line before the declaration",
          "[sourcetext][leadingComment]")
{
    const QStringList lines = {
        QStringLiteral("// Comment"),
        QStringLiteral(""),
        QStringLiteral("class A {};"),
    };
    REQUIRE(leadingComment(lines, 2) == QStringLiteral("Comment"));
}

// NOTE: the doc comment on leadingComment() says "a single blank line ... is
// tolerated", but the implementation's blank-skipping loop is guarded by
// `collected.isEmpty()`, which is trivially true at that point — so the guard is
// dead and ANY number of blank lines is skipped. This is preserved verbatim from
// repoanalyzer.cpp; these cases pin the behaviour that actually exists rather
// than the behaviour the comment describes.
TEST_CASE("leadingComment skips any run of blank lines (documented-vs-actual)",
          "[sourcetext][leadingComment]")
{
    const QStringList two = {
        QStringLiteral("// Comment"),
        QStringLiteral(""),
        QStringLiteral(""),
        QStringLiteral("class A {};"),
    };
    REQUIRE(leadingComment(two, 3) == QStringLiteral("Comment"));

    const QStringList three = {
        QStringLiteral("// Comment"),
        QStringLiteral("   "),
        QStringLiteral(""),
        QStringLiteral("\t"),
        QStringLiteral("class A {};"),
    };
    REQUIRE(leadingComment(three, 4) == QStringLiteral("Comment"));
}

TEST_CASE("leadingComment yields empty when code intervenes",
          "[sourcetext][leadingComment]")
{
    const QStringList lines = {
        QStringLiteral("// Comment"),
        QStringLiteral("int unrelated;"),
        QStringLiteral("class A {};"),
    };
    REQUIRE(leadingComment(lines, 2).isEmpty());
}

TEST_CASE("leadingComment is empty at declLine 0 and reads nothing out of bounds",
          "[sourcetext][leadingComment]")
{
    const QStringList lines = {
        QStringLiteral("class A {};"),
    };
    REQUIRE(leadingComment(lines, 0).isEmpty());

    const QStringList empty;
    REQUIRE(leadingComment(empty, 0).isEmpty());
}

TEST_CASE("matchingBrace finds the index one past the closing brace",
          "[sourcetext][matchingBrace]")
{
    const QString text = QStringLiteral("class A { int x; };");
    const int open = text.indexOf(QLatin1Char('{'));
    const int end  = matchingBrace(text, open);

    REQUIRE(text.at(end - 1) == QLatin1Char('}'));
    REQUIRE(end == text.lastIndexOf(QLatin1Char('}')) + 1);
}

TEST_CASE("matchingBrace skips nested braces", "[sourcetext][matchingBrace]")
{
    const QString text = QStringLiteral("struct S { struct N { int a; }; int b; }; tail");
    const int open = text.indexOf(QLatin1Char('{'));
    const int end  = matchingBrace(text, open);

    REQUIRE(text.at(end - 1) == QLatin1Char('}'));
    REQUIRE(end == text.lastIndexOf(QLatin1Char('}')) + 1);
    REQUIRE(text.mid(end) == QStringLiteral("; tail"));
}

TEST_CASE("matchingBrace returns text.size() when the brace is unmatched",
          "[sourcetext][matchingBrace]")
{
    const QString text = QStringLiteral("class A { int x;");
    REQUIRE(matchingBrace(text, text.indexOf(QLatin1Char('{'))) == text.size());
}

TEST_CASE("matchingBrace guards openBrace that is out of range or not a brace",
          "[sourcetext][matchingBrace]")
{
    const QString text = QStringLiteral("class A { int x; };");
    REQUIRE(matchingBrace(text, 0)   == text.size());   // not a '{'
    REQUIRE(matchingBrace(text, -1)  == text.size());   // before the start
    REQUIRE(matchingBrace(text, 999) == text.size());   // past the end
}

TEST_CASE("matchingBrace round-trips a class body via the documented mid()",
          "[sourcetext][matchingBrace]")
{
    const QString text = blankC(QStringLiteral(
        "class A { int x; void f(); struct N { int y; }; };"));
    const int open = text.indexOf(QLatin1Char('{'));
    const QString body = text.mid(open + 1, matchingBrace(text, open) - open - 2);

    REQUIRE(body == QStringLiteral(" int x; void f(); struct N { int y; }; "));
    REQUIRE(body.count(QLatin1Char('{')) == body.count(QLatin1Char('}')));
}

TEST_CASE("lineOf reports 0-based lines", "[sourcetext][lineOf]")
{
    const QString text = QStringLiteral("a\nbb\nccc\n");

    REQUIRE(lineOf(text, 0) == 0);                                  // start of line 0
    REQUIRE(lineOf(text, 1) == 0);                                  // the '\n' itself
    REQUIRE(lineOf(text, text.indexOf(QStringLiteral("bb")))  == 1);
    REQUIRE(lineOf(text, text.indexOf(QStringLiteral("ccc"))) == 2);
    REQUIRE(lineOf(text, text.size()) == 3);                        // past the last '\n'
}
