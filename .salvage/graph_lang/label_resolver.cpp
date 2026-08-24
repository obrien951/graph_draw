#include "graph_lang/label_resolver.h"
#include <algorithm>

namespace graph_lang {

std::string LabelResolver::resolve(const std::vector<std::string>& candidates,
                                   const std::unordered_set<std::string>& known_labels) {
    // First pass: find the first candidate that is unique repo-wide
    for (const auto& candidate : candidates) {
        if (is_unique(candidate, known_labels)) {
            return candidate;
        }
    }

    // Second pass: return the longest candidate if none are unique
    return longest(candidates);
}

bool LabelResolver::is_unique(const std::string& label,
                              const std::unordered_set<std::string>& known_labels) {
    return known_labels.find(label) == known_labels.end();
}

std::string LabelResolver::longest(const std::vector<std::string>& candidates) {
    if (candidates.empty()) {
        return "";
    }

    std::string longest = candidates[0];
    for (size_t i = 1; i < candidates.size(); ++i) {
        if (candidates[i].length() > longest.length()) {
            longest = candidates[i];
        }
    }
    return longest;
}

} // namespace graph_lang
