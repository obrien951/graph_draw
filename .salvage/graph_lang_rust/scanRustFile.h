#pragma once

#include <string>
#include <vector>

namespace graph_lang_rust {

/**
 * Scans blanked Rust text for items at item depth using brace-depth analysis.
 * 
 * Items are recognized only at item depth, so fns inside function bodies
 * and impls inside macro_rules! bodies never reach the graph.
 * 
 * impl headers are scanned with a real brace-depth scan (not regex):
 *   - find the first '{' not nested in <>, (), or []
 *   - strip leading generics
 *   - split on a depth-0 ' for '
 *   - strip a trailing where clause
 */
std::vector<std::string> scanRustFile(const std::string& blankedText);

} // namespace graph_lang_rust
