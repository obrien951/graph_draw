#include "sourcetext.h"
#include <algorithm>
#include <cctype>

namespace sourcetext {

namespace {

/**
 * @brief Strips leading whitespace and comment markers from a comment block.
 *
 * @param comment The raw comment block including markers.
 * @return The comment text with markers and leading whitespace removed.
 */
std::string stripMarkers(const std::string& comment) {
    std::string result;
    bool inBlockComment = false;
    bool inLineComment = false;

    for (size_t i = 0; i < comment.size(); ++i) {
        char c = comment[i];

        if (inBlockComment) {
            if (c == '*' && i + 1 < comment.size() && comment[i + 1] == '/') {
                inBlockComment = false;
                ++i; // skip the '/'
            }
            continue;
        }

        if (inLineComment) {
            continue; // skip rest of line comment
        }

        if (c == '/' && i + 1 < comment.size() && comment[i + 1] == '/') {
            inLineComment = true;
            continue;
        }

        if (c == '/' && i + 1 < comment.size() && comment[i + 1] == '*') {
            inBlockComment = true;
            ++i; // skip the '*'
            continue;
        }

        // Skip leading whitespace
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }

        result += c;
    }

    return result;
}

} // anonymous namespace

std::string leadingComment(const std::string& source, size_t declarationPos) {
    if (declarationPos == 0) {
        return {};
    }

    // Scan backwards to find the start of the comment block
    size_t commentStart = declarationPos;
    size_t i = declarationPos;

    while (i > 0) {
        --i;
        char c = source[i];

        if (c == '\n') {
            // Found a newline — this is the end of the comment block
            commentStart = i + 1;
            break;
        }

        if (c == '\r') {
            // Found a carriage return — this is the end of the comment block
            commentStart = i + 1;
            break;
        }

        // If we hit a non-whitespace character before a newline,
        // the comment block starts here
        if (!std::isspace(static_cast<unsigned char>(c))) {
            commentStart = i + 1;
            break;
        }
    }

    // Extract the comment block
    std::string commentBlock = source.substr(commentStart, declarationPos - commentStart);

    // Strip markers
    return stripMarkers(commentBlock);
}

} // namespace sourcetext
