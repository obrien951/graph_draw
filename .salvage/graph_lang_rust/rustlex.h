#ifndef GRAPH_LANG_RUST_RUSTLEX_H
#define GRAPH_LANG_RUST_RUSTLEX_H

#include <string>

namespace rustlex {

/**
 * Strips the leading doc comment prefix from a Rust doc string.
 * 
 * Rules:
 * - Skips blank lines and #[attribute] lines before the doc comment.
 * - Strips 3 characters for /// and //! (not 2).
 * - Returns the cleaned doc string.
 * 
 * Example:
 *   Input:  "/// A doc comment.\n///\n/// More text.\n#[derive(Debug)]\n/// Item"
 *   Output: "A doc comment.\n\nMore text.\nItem"
 */
std::string leadingDoc(const std::string& input);

} // namespace rustlex

#endif // GRAPH_LANG_RUST_RUSTLEX_H
