#include "graph_lang/labelresolver.h"
#include <algorithm>
#include <sstream>

namespace graph_lang {

void LabelResolver::observe(const std::vector<Item>& items) {
    for (const auto& item : items) {
        for (const auto& candidate : item.candidates) {
            // Only register if not already registered
            if (!labelToSource_.count(candidate.label)) {
                labelToSource_[candidate.label] = candidate.source;
                labels_.push_back(candidate.label);
            }
        }
    }

    // Sort labels short to long
    std::sort(labels_.begin(), labels_.end(),
              [](const std::string& a, const std::string& b) {
                  return a.size() < b.size();
              });
}

std::vector<std::string> LabelResolver::getLabels() const {
    return labels_;
}

std::string LabelResolver::getSource(const std::string& label) const {
    auto it = labelToSource_.find(label);
    if (it != labelToSource_.end()) {
        return it->second;
    }
    return "";
}

bool LabelResolver::hasLabel(const std::string& label) const {
    return labelToSource_.count(label) > 0;
}

} // namespace graph_lang
