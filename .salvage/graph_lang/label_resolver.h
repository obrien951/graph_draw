#ifndef GRAPH_LANG_LABEL_RESOLVER_H
#define GRAPH_LANG_LABEL_RESOLVER_H

#include <string>
#include <vector>
#include <unordered_set>

namespace graph_lang {

/**
 * @brief Resolves labels by preferring unique candidates, falling back to longest.
 *
 * NEW (B8). Pass two: the first candidate that is unique repo-wide,
 * else the longest.
 */
class LabelResolver {
public:
    /**
     * @brief Resolve a label from candidates.
     * @param candidates List of candidate labels to choose from.
     * @param known_labels Set of labels already known to exist in the repo.
     * @return The first unique label, or the longest if none are unique.
     */
    static std::string resolve(const std::vector<std::string>& candidates,
                               const std::unordered_set<std::string>& known_labels);

private:
    /**
     * @brief Check if a label is unique (not in known_labels).
     */
    static bool is_unique(const std::string& label,
                          const std::unordered_set<std::string>& known_labels);

    /**
     * @brief Get the longest label from candidates.
     */
    static std::string longest(const std::vector<std::string>& candidates);
};

} // namespace graph_lang

#endif // GRAPH_LANG_LABEL_RESOLVER_H
