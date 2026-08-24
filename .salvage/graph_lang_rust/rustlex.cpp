#include "rustlex.h"
#include <algorithm>
#include <cctype>

namespace rustlex {

namespace {

/**
 * Checks if a line is a blank line or a #[attribute] line.
 */
bool is_skippable_line(const std::string& line) {
    // Blank line
    if (line.empty() || std::all_of(line.begin(), line.end(),
                                     [](unsigned char c) { return std::isspace(c); })) {
        return true;
    }
    // #[attribute] line
    if (line.size() >= 2 && line[0] == '#' && line[1] == '[') {
        return true;
    }
    return false;
}

/**
 * Strips the leading /// or //! from a doc line.
 * Removes exactly 3 characters (the /// or //! prefix).
 */
std::string strip_doc_prefix(const std::string& line) {
    if (line.size() < 3) {
        return line;
    }
    // Strip ///
    if (line[0] == '/' && line[1] == '/' && line[2] == '/') {
        return line.substr(3);
    }
    // Strip //!
    if (line[0] == '/' && line[1] == '/' && line[2] == '!') {
        return line.substr(3);
    }
    return line;
}

} // anonymous namespace

std::string leadingDoc(const std::string& input) {
    if (input.empty()) {
        return "";
    }

    std::string result;
    bool found_doc = false;

    for (const auto& line : input) {
        // Skip blank lines and #[attribute] lines
        if (is_skippable_line(line)) {
            continue;
        }

        // Strip the doc prefix (/// or //!) — 3 characters, not 2
        if (!found_doc) {
            result += strip_doc_prefix(line);
            found_doc = true;
        } else {
            result += line;
        }
    }

    return result;
}

} // namespace rustlex
