#pragma once
#include <QString>

// rustlex::blank: Rust-dialect blanking as a forward scanner.
// Handles nested block comments via depth counter, raw strings (r"", r#""#, r##...##),
// and line comments. Returns a QString where all comments and string contents are
// replaced with spaces (newlines preserved) so line numbers stay aligned.
QString blank(const QString& text);
