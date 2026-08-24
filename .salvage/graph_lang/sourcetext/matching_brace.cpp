#include "sourcetext/matching_brace.h"
#include <stdexcept>

namespace sourcetext {

namespace {

/**
 * @brief Skips over a C++ comment starting at the given position.
 * @param source The source string.
 * @param pos The position where the comment starts.
 * @return The position after the comment ends.
 */
std::size_t skipComment(const std::string& source, std::size_t pos) {
    if (pos + 1 >= source.size()) {
        return pos;
    }

    // Single-line comment: //
    if (source[pos] == '/' && source[pos + 1] == '/') {
        while (pos < source.size() && source[pos] != '\n') {
            ++pos;
        }
        return pos;
    }

    // Multi-line comment: /* */
    if (source[pos] == '/' && source[pos + 1] == '*') {
        pos += 2;
        while (pos + 1 < source.size() && !(source[pos] == '*' && source[pos + 1] == '/')) {
            ++pos;
        }
        return pos + 2; // Skip the closing */
    }

    return pos;
}

/**
 * @brief Skips over a string literal starting at the given position.
 * @param source The source string.
 * @param pos The position of the opening quote.
 * @return The position after the closing quote.
 * @throws std::out_of_range if the string is unterminated.
 */
std::size_t skipStringLiteral(const std::string& source, std::size_t pos) {
    if (pos >= source.size()) {
        throw std::out_of_range("Unterminated string literal");
    }

    char quote = source[pos]; // ' or "
    ++pos;

    while (pos < source.size()) {
        if (source[pos] == '\\') {
            // Skip escaped character
            ++pos;
            if (pos >= source.size()) {
                throw std::out_of_range("Unterminated string literal");
            }
            ++pos;
        } else if (source[pos] == quote) {
            return pos + 1;
        } else {
            ++pos;
        }
    }

    throw std::out_of_range("Unterminated string literal");
}

} // anonymous namespace

std::size_t matchingBrace(const std::string& source, std::size_t openIndex) {
    if (openIndex >= source.size() || source[openIndex] != '{') {
        throw std::out_of_range("Expected '{' at index " + std::to_string(openIndex));
    }

    int depth = 1;
    std::size_t pos = openIndex + 1;

    while (pos < source.size() && depth > 0) {
        // Skip comments
        pos = skipComment(source, pos);

        // Skip string literals
        if (pos < source.size() && (source[pos] == '"' || source[pos] == '\'')) {
            pos = skipStringLiteral(source, pos);
            continue;
        }

        if (source[pos] == '{') {
            ++depth;
        } else if (source[pos] == '}') {
            --depth;
        }

        ++pos;
    }

    if (depth > 0) {
        throw std::out_of_range("No matching closing brace found");
    }

    return pos - 1; // Return index of the matching '}'
}

} // namespace sourcetext
