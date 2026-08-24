#ifndef GRAPH_LANG_LABELRESOLVER_H
#define GRAPH_LANG_LABELRESOLVER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace graph_lang {

/**
 * @brief Collects and orders candidate labels for graph nodes.
 * 
 * The observe method registers every item's candidate labels,
 * ordered short to long.
 */
class LabelResolver {
public:
    /**
     * @brief A candidate label for a node.
     */
    struct CandidateLabel {
        std::string label;
        std::string source;  // where the label came from
    };

    /**
     * @brief Represents an item that may have multiple candidate labels.
     */
    struct Item {
        std::string name;
        std::vector<CandidateLabel> candidates;
    };

    /**
     * @brief Observe items and register their candidate labels,
     *        ordered short to long.
     * 
     * @param items A collection of items with candidate labels.
     */
    void observe(const std::vector<Item>& items);

    /**
     * @brief Get the ordered list of registered labels.
     * 
     * Labels are returned in order: short to long.
     * 
     * @return A vector of labels ordered by length.
     */
    std::vector<std::string> getLabels() const;

    /**
     * @brief Get the source for a given label.
     * 
     * @param label The label to look up.
     * @return The source of the label, or empty string if not found.
     */
    std::string getSource(const std::string& label) const;

    /**
     * @brief Check if a label has been registered.
     * 
     * @param label The label to check.
     * @return true if the label is registered, false otherwise.
     */
    bool hasLabel(const std::string& label) const;

private:
    std::unordered_map<std::string, std::string> labelToSource_;
    std::vector<std::string> labels_;
};

} // namespace graph_lang

#endif // GRAPH_LANG_LABELRESOLVER_H
