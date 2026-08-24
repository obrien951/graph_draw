#include "toml_document.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace graph_lang_rust {

TomlDocument::TomlDocument(const std::string& content)
    : content_(content), keys_(), values_() {
    parse(content);
}

std::string TomlDocument::trim(const std::string& s) const {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string TomlDocument::unquote(const std::string& s) const {
    std::string result = trim(s);
    if (result.size() >= 2) {
        if ((result.front() == '"' && result.back() == '"') ||
            (result.front() == '\'' && result.back() == '\'')) {
            result = result.substr(1, result.size() - 2);
        }
    }
    return result;
}

void TomlDocument::parse(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    std::string currentKey;
    std::string currentValue;
    bool inArray = false;
    bool inTableArray = false;
    bool inTable = false;
    size_t arrayIndex = 0;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);

        // Skip empty lines and comments
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        // Handle array-of-tables: [[key]]
        if (trimmed.size() >= 2 && trimmed[0] == '[' && trimmed[1] == '[') {
            size_t endBracket = trimmed.find(']', trimmed.size() - 2);
            if (endBracket != std::string::npos) {
                std::string key = trim(trimmed.substr(2, endBracket - 2));
                inTableArray = true;
                inTable = false;
                currentKey = key;
                currentValue = "";
                arrayIndex = 0;
                continue;
            }
        }

        // Handle regular table: [key]
        if (trimmed.size() >= 2 && trimmed[0] == '[' && trimmed[1] == ']') {
            size_t endBracket = trimmed.find(']', trimmed.size() - 2);
            if (endBracket != std::string::npos) {
                std::string key = trim(trimmed.substr(2, endBracket - 2));
                inTableArray = false;
                inTable = true;
                currentKey = key;
                currentValue = "";
                continue;
            }
        }

        // Handle key = value
        size_t eqPos = trimmed.find('=');
        if (eqPos != std::string::npos) {
            std::string key = trim(trimmed.substr(0, eqPos));
            std::string value = trim(trimmed.substr(eqPos + 1));

            if (inTableArray) {
                // Array-of-table entry: [[key]] { key = value }
                std::string dottedKey = currentKey + "." + std::to_string(arrayIndex) + "." + key;
                values_[dottedKey] = unquote(value);
                currentValue += key + "=" + value + "; ";
                arrayIndex++;
            } else if (inTable) {
                // Regular table entry: [key] { key = value }
                std::string dottedKey = currentKey + "." + key;
                values_[dottedKey] = unquote(value);
                currentValue += key + "=" + value + "; ";
            } else {
                // Top-level key = value
                values_[key] = unquote(value);
                currentValue += key + "=" + value + "; ";
            }
            continue;
        }

        // Handle array entries: key = [ value1, value2 ]
        if (trimmed.find('[') != std::string::npos && trimmed.find(']') != std::string::npos) {
            size_t openBracket = trimmed.find('[');
            size_t closeBracket = trimmed.find(']');
            if (openBracket != std::string::npos && closeBracket != std::string::npos && closeBracket > openBracket) {
                std::string key = trim(trimmed.substr(0, openBracket));
                std::string value = trim(trimmed.substr(openBracket + 1, closeBracket - openBracket - 1));
                std::string dottedKey = currentKey + "." + key;
                values_[dottedKey] = unquote(value);
                currentValue += key + "=" + value + "; ";
                continue;
            }
        }

        // Handle array-of-tables: [[key]] { key = value }
        if (inTableArray) {
            std::string key = trim(trimmed);
            if (!key.empty() && key[0] != '[') {
                std::string dottedKey = currentKey + "." + std::to_string(arrayIndex) + "." + key;
                values_[dottedKey] = unquote(key);
                currentValue += key + "=" + key + "; ";
                arrayIndex++;
            }
            continue;
        }

        // Handle regular table: [key] { key = value }
        if (inTable) {
            std::string key = trim(trimmed);
            if (!key.empty() && key[0] != '[') {
                std::string dottedKey = currentKey + "." + key;
                values_[dottedKey] = unquote(key);
                currentValue += key + "=" + key + "; ";
            }
            continue;
        }
    }

    // Build keys in insertion order
    for (const auto& pair : values_) {
        keys_.push_back(pair.first);
    }
}

size_t TomlDocument::tableArrayCount() const {
    size_t count = 0;
    for (const auto& key : keys_) {
        if (key.size() >= 2 && key[0] == '[' && key[1] == '[') {
            count++;
        }
    }
    return count;
}

std::vector<std::string> TomlDocument::keys() const {
    return keys_;
}

std::string TomlDocument::get(const std::string& path) const {
    auto it = values_.find(path);
    if (it != values_.end()) {
        return it->second;
    }
    return "";
}

bool TomlDocument::hasKey(const std::string& path) const {
    return values_.find(path) != values_.end();
}

} // namespace graph_lang_rust
