#include "graph_lang/ir/type.h"

namespace ir {

Type::Type(const std::string& label, const std::string& description,
           const std::string& owning_module_label)
    : label_(label),
      description_(description),
      owning_module_label_(owning_module_label) {
}

void Type::add_base_label(const std::string& label) {
    base_labels_.push_back(label);
}

void Type::add_uses_label(const std::string& label) {
    uses_labels_.push_back(label);
}

void Type::add_method(Function method) {
    methods_.push_back(std::move(method));
}

} // namespace ir
