#pragma once

#include <string>
#include <vector>

namespace sourcetext {

/**
 * @brief Returns the contiguous comment block immediately above a declaration,
 * stripped of comment markers.
 *
 * Scans backwards from `declarationPos` to find the start of the comment block.
 * Strips leading `//` and `/* */` markers and returns the raw comment text.
 *
 * @param source The full source text.
 * @param declarationPos The position in source where the declaration begins.
 * @return The comment block text with markers removed, or empty string if none found.
 */
std::string leadingComment(const std::string& source, size_t declarationPos);

} // namespace sourcetext
