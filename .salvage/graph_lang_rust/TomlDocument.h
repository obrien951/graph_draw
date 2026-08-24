#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace graph_lang_rust {

class TomlDocument {
public:
    TomlDocument();
    ~TomlDocument();

    // Parse TOML text into a dotted-path representation.
    // Malformed lines are skipped rather than fatal.
    // Multi-line """ strings are skipped over without desynchronizing tables.
    void parseText(const std::string& text);

    // Get all keys in insertion order.
    std::vector<std::string> keys() const;

    // Get a value by dotted path (e.g., "package.name", "bin[1].name").
    // Returns empty string if key not found.
    std::string get(const std::string& path) const;

    // Check if a key exists.
    bool hasKey(const std::string& path) const;

    // Get the value as a string (for simple key-value pairs).
    std::string getValue(const std::string& path) const;

private:
    struct Entry {
        std::string key;
        std::string value;
    };

    struct Table {
        std::string name;
        std::vector<Entry> entries;
    };

    std::vector<Table> tables_;
    std::unordered_map<std::string, std::string> flat_;

    // Helper to extract string value, handling quotes and escapes.
    std::string extractString(const std::string& text, size_t& pos) const;

    // Helper to extract a dotted path from a key.
    std::vector<std::string> splitPath(const std::string& key) const;

    // Helper to join path components.
    std::string joinPath(const std::vector<std::string>& parts) const;
};

} // namespace graph_lang_rust
