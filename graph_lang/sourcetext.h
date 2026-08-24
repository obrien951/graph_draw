#pragma once
#include <QString>
#include <QStringList>

// Language-neutral helpers for scanning C-family source text.
//
// These were extracted verbatim from graph_analyze/repoanalyzer.cpp (B1 of
// IMPLEMENTATION_PLAN.md) so the C++ and Rust analyzers can share them. Behaviour
// is deliberately unchanged: tests/test_repoanalyzer.cpp is the regression
// contract for the extraction and must keep passing unedited.
namespace sourcetext {

// Replace comments and string/char literals with spaces so declaration matching
// never trips over commented-out or quoted code.
//
// INVARIANT: blankC(s).size() == s.size(), and every '\n' in s is preserved at
// its original offset. This is load-bearing — callers index the *raw* text using
// offsets found in the blanked text — and the Rust lexer must satisfy it too.
QString blankC(const QString& s);

// The contiguous comment block immediately above line declLine (0-based) in
// rawLines, stripped of comment markers and simplified. A single blank line
// between the comment and the declaration is tolerated. Empty if there is none.
QString leadingComment(const QStringList& rawLines, int declLine);

// Index just past the '}' matching the '{' at openBrace.
//
// text is expected to be blanked (see blankC) so braces inside literals cannot
// corrupt the depth count. Returns text.size() when the brace is unmatched,
// which mirrors the loop this replaced. Callers extracting a body want
// text.mid(openBrace + 1, matchingBrace(text, openBrace) - openBrace - 2).
int matchingBrace(const QString& text, int openBrace);

// 0-based line number containing the given character offset.
int lineOf(const QString& text, int offset);

} // namespace sourcetext
