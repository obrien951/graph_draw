#ifndef GRAPH_LANG_RUST_TOML_DOCUMENT_H
#define GRAPH_LANG_RUST_TOML_DOCUMENT_H

#include <string>
#include <vector>
#include <unordered_map>

namespace graph_lang_rust {

/**
 * @brief A read-only Cargo.toml reader flattened to dotted paths.
 *
 * Provides insertion-ordered storage so keys() follows file order
 * and output stays deterministic.
 */
class TomlDocument {
public:
    /**
     * @brief Construct a TomlDocument from a TOML string.
     * @param content The raw TOML content.
     */
    explicit TomlDocument(const std::string& content);

    /**
     * @brief Get the number of [[array-of-table]] entries.
     *
     * Counts entries like [[bin]], [[dependencies]], etc.
     * Needed for multiple [[bin]] sections in Cargo.toml.
     *
     * @return The count of array-of-table entries.
     */
    size_t tableArrayCount() const;

    /**
     * @brief Get all keys in insertion order.
     * @return Vector of key strings in file order.
     */
    std::vector<std::string> keys() const;

    /**
     * @brief Get a value by dotted path (e.g., "bin.0.name").
     * @param path The dotted path to the value.
     * @return The value string, or empty string if not found.
     */
    std::string get(const std::string& path) const;

    /**
     * @brief Check if a key exists at the given dotted path.
     * @param path The dotted path to check.
     * @return true if the path exists.
     */
    bool hasKey(const std::string& path) const;

private:
    std::string content_;
    std::vector<std::string> keys_;
    std::unordered_map<std::string, std::string> values_;

    void parse(const std::string& content);
    void parseValue(const std::string& key, const std::string& value);
    void parseArray(const std::string& key, const std::string& value);
    void parseTableArray(const std::string& key, const std::string& value);
    std::string trim(const std::string& s) const;
    std::string unquote(const std::string& s) const;
};

} // namespace graph_lang_rust

#endif // GRAPH_LANG_RUST_TOML_DOCUMENT_H
