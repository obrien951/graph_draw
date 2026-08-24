#ifndef GRAPH_LANG_SOURCETEXT_MATCHING_BRACE_H
#define GRAPH_LANG_SOURCETEXT_MATCHING_BRACE_H

#include <cstddef>
#include <string>
#include <stdexcept>

namespace sourcetext {

/**
 * @brief Finds the index just past the closing brace matching a given opening brace.
 *
 * Skips over C++ comments (// and /* */) and string literals ("...") to avoid
 * false matches. The input index must point to an opening '{' character.
 *
 * @param source The source code string to search in.
 * @param openIndex The index of the opening '{' character.
 * @return The index of the character immediately after the matching '}'.
 * @throws std::out_of_range if no matching closing brace is found.
 */
std::size_t matchingBrace(const std::string& source, std::size_t openIndex);

} // namespace sourcetext

#endif // GRAPH_LANG_SOURCETEXT_MATCHING_BRACE_H
