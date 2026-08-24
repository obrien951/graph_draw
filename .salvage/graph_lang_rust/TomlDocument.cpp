#include "TomlDocument.h"
#include <sstream>
#include <cctype>
#include <algorithm>

namespace graph_lang_rust {

TomlDocument::TomlDocument() = default;
TomlDocument::~TomlDocument() = default;

std::string TomlDocument::extractString(const std::string& text, size_t& pos) const {
    // Handle multi-line strings: """...""" or '''...'''
    if (pos < text.size() && text[pos] == '"') {
        if (pos + 2 < text.size() && text[pos + 1] == '"') {
            // Multi-line string: """..."""
            pos += 3;
            std::string result;
            while (pos < text.size()) {
                if (text[pos] == '"') {
                    if (pos + 2 < text.size() && text[pos + 1] == '"') {
                        // End of multi-line string
                        pos += 3;
                        return result;
                    }
                }
                result += text[pos];
                pos++;
            }
            return result;
        }
    }

    // Handle single-line strings: "..." or '...'
    if (pos < text.size() && (text[pos] == '"' || text[pos] == '\'')) {
        char quote = text[pos];
        pos++;
        std::string result;
        while (pos < text.size()) {
            if (text[pos] == quote) {
                // Check for escaped quote
                if (pos > 0 && text[pos - 1] == '\\') {
                    result += quote;
                    pos++;
                    continue;
                }
                pos++;
                return result;
            }
            result += text[pos];
            pos++;
        }
        return result;
    }

    // No string, return empty
    return "";
}

std::vector<std::string> TomlDocument::splitPath(const std::string& key) const {
    std::vector<std::string> parts;
    std::string current;
    for (size_t i = 0; i < key.size(); i++) {
        if (key[i] == '.') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current += key[i];
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::string TomlDocument::joinPath(const std::vector<std::string>& parts) const {
    std::string result;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) result += ".";
        result += parts[i];
    }
    return result;
}

void TomlDocument::parseText(const std::string& text) {
    tables_.clear();
    flat_.clear();

    size_t pos = 0;
    while (pos < text.size()) {
        // Skip whitespace
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            pos++;
        }
        if (pos >= text.size()) break;

        // Skip comments (# ...)
        if (text[pos] == '#') {
            while (pos < text.size() && text[pos] != '\n') pos++;
            continue;
        }

        // Skip empty lines
        if (text[pos] == '\n') {
            pos++;
            continue;
        }

        // Check for multi-line string
        if (pos + 2 < text.size() && text[pos] == '"' && text[pos + 1] == '"') {
            if (pos + 3 < text.size() && text[pos + 2] == '"') {
                // Multi-line string: """..."""
                pos += 3;
                while (pos < text.size()) {
                    if (text[pos] == '"') {
                        if (pos + 2 < text.size() && text[pos + 1] == '"') {
                            pos += 3;
                            break;
                        }
                    }
                    pos++;
                }
                continue;
            }
        }

        // Check for table header: [section] or [section.subsection]
        if (text[pos] == '[') {
            pos++; // skip [
            std::string sectionName;
            while (pos < text.size() && text[pos] != ']') {
                sectionName += text[pos];
                pos++;
            }
            if (pos < text.size() && text[pos] == ']') pos++; // skip ]

            // Skip whitespace and optional inline table
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) pos++;
            if (pos < text.size() && text[pos] == '=') pos++; // skip =
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) pos++;

            // Skip inline table if present (e.g., [section = { a = 1 }])
            if (pos < text.size() && text[pos] == '{') {
                pos++; // skip {
                while (pos < text.size()) {
                    if (text[pos] == '}') {
                        pos++;
                        break;
                    }
                    pos++;
                }
            }

            // Skip to end of line
            while (pos < text.size() && text[pos] != '\n') pos++;
            if (pos < text.size()) pos++; // skip \n

            // Add table
            Table table;
            table.name = sectionName;
            tables_.push_back(table);
            continue;
        }

        // Check for key-value pair: key = value
        if (text[pos] != '\n' && text[pos] != '\r') {
            std::string key;
            while (pos < text.size() && text[pos] != '=' && text[pos] != '\n' && text[pos] != '\r') {
                key += text[pos];
                pos++;
            }

            if (pos < text.size() && text[pos] == '=') {
                pos++; // skip =
                while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) pos++;

                std::string value;
                if (pos < text.size() && (text[pos] == '"' || text[pos] == '\'')) {
                    value = extractString(text, pos);
                } else {
                    // Raw value (number, boolean, array, inline table, etc.)
                    while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') {
                        value += text[pos];
                        pos++;
                    }
                }

                // Trim whitespace from value
                size_t start = 0, end = value.size();
                while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) start++;
                while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
                value = value.substr(start, end - start);

                // Store flat mapping
                std::string flatKey = joinPath(splitPath(key));
                flat_[flatKey] = value;

                // Skip to end of line
                while (pos < text.size() && text[pos] != '\n') pos++;
                if (pos < text.size()) pos++; // skip \n
            } else {
                // Malformed line - skip to end of line
                while (pos < text.size() && text[pos] != '\n') pos++;
                if (pos < text.size()) pos++; // skip \n
            }
        }
    }
}

std::vector<std::string> TomlDocument::keys() const {
    std::vector<std::string> result;
    for (const auto& entry : flat_) {
        result.push_back(entry.first);
    }
    return result;
}

std::string TomlDocument::get(const std::string& path) const {
    auto it = flat_.find(path);
    if (it != flat_.end()) {
        return it->second;
    }
    return "";
}

bool TomlDocument::hasKey(const std::string& path) const {
    return flat_.find(path) != flat_.end();
}

std::string TomlDocument::getValue(const std::string& path) const {
    auto it = flat_.find(path);
    if (it != flat_.end()) {
        return it->second;
    }
    return "";
}

} // namespace graph_lang_rust
