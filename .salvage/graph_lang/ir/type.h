#ifndef GRAPH_LANG_IR_TYPE_H
#define GRAPH_LANG_IR_TYPE_H

#include <string>
#include <vector>
#include <memory>

#include "graph_lang/ir/module.h"
#include "graph_lang/ir/repo.h"

namespace ir {

/**
 * @brief A Type node in the IR representing a C++ class/struct or Rust struct/enum/trait/union.
 *
 * A Type node captures the label, description, owning module, base types,
 * used types, and methods of a type definition.
 */
class Type {
public:
    /**
     * @brief Construct a new Type node.
     * @param label The unique identifier for this type.
     * @param description A human-readable description of the type.
     * @param owning_module_label The label of the module that owns this type.
     */
    Type(const std::string& label, const std::string& description,
         const std::string& owning_module_label);

    /**
     * @brief Get the label of this type.
     * @return The type label.
     */
    const std::string& label() const { return label_; }

    /**
     * @brief Get the description of this type.
     * @return The type description.
     */
    const std::string& description() const { return description_; }

    /**
     * @brief Get the owning module label.
     * @return The owning module label.
     */
    const std::string& owning_module_label() const { return owning_module_label_; }

    /**
     * @brief Get the base types of this type.
     * @return A list of base type labels.
     */
    const std::vector<std::string>& base_labels() const { return base_labels_; }

    /**
     * @brief Get the types used by this type.
     * @return A list of used type labels.
     */
    const std::vector<std::string>& uses_labels() const { return uses_labels_; }

    /**
     * @brief Get the methods of this type.
     * @return A list of methods belonging to this type.
     */
    const std::vector<Function>& methods() const { return methods_; }

    /**
     * @brief Add a base type to this type.
     * @param label The label of the base type.
     */
    void add_base_label(const std::string& label);

    /**
     * @brief Add a used type to this type.
     * @param label The label of the used type.
     */
    void add_uses_label(const std::string& label);

    /**
     * @brief Add a method to this type.
     * @param method The method to add.
     */
    void add_method(Function method);

    /**
     * @brief Check if this type has any base types.
     * @return true if this type has base types.
     */
    bool is_interface() const { return base_labels_.empty(); }

private:
    std::string label_;
    std::string description_;
    std::string owning_module_label_;
    std::vector<std::string> base_labels_;
    std::vector<std::string> uses_labels_;
    std::vector<Function> methods_;
};

} // namespace ir

#endif // GRAPH_LANG_IR_TYPE_H
