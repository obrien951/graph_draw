#pragma once

#include <string>
#include <vector>
#include <regex>
#include <memory>
#include "graph_core/graphscene.h"

namespace graph_lang_cpp {

/**
 * @brief Analyzes a C++ codebase and produces a dependency graph.
 * 
 * Parses CMake targets, class/struct definitions, member functions,
 * and includes to build a complete dependency graph.
 */
class CppAnalyzer {
public:
    /**
     * @brief Analyzes the given source file and returns a GraphScene.
     * 
     * @param isCppSource Whether the file is a C++ source file.
     * @param isGeneratedSource Whether the file is a generated source file.
     * @param isNonFunctionWord Whether the line is a non-function word (skip).
     * @param extractMethods Whether to extract method definitions.
     * @param baseIdentifier The base identifier for the current module.
     * @param readCommandArgs The command-line arguments for parsing.
     * @param classRegex Regex pattern for matching class/struct definitions.
     * @param includeRegex Regex pattern for matching include directives.
     * @param cmakeTargetRegex Regex pattern for matching CMake targets.
     * @param cmakeTargetPrefix Prefix for CMake target names.
     * @param cmakeTargetSuffix Suffix for CMake target names.
     * @param includePathPrefix Prefix for include paths.
     * @param includePathSuffix Suffix for include paths.
     * @return GraphScene representing the parsed dependency graph.
     */
    GraphScene analyze(
        bool isCppSource,
        bool isGeneratedSource,
        bool isNonFunctionWord,
        bool extractMethods,
        const std::string& baseIdentifier,
        const std::vector<std::string>& readCommandArgs,
        const std::regex& classRegex,
        const std::regex& includeRegex,
        const std::regex& cmakeTargetRegex,
        const std::string& cmakeTargetPrefix,
        const std::string& cmakeTargetSuffix,
        const std::string& includePathPrefix,
        const std::string& includePathSuffix
    ) const;

private:
    /**
     * @brief Parses a CMake target line and extracts module information.
     * 
     * @param line The CMake target line to parse.
     * @return ModuleInfo containing the parsed module information.
     */
    ModuleInfo parseCMakeTarget(const std::string& line) const;

    /**
     * @brief Parses a class/struct definition and extracts type information.
     * 
     * @param line The class/struct definition line to parse.
     * @return ClassInfo containing the parsed type information.
     */
    ClassInfo parseClassDefinition(const std::string& line) const;

    /**
     * @brief Parses a member function definition and extracts method information.
     * 
     * @param line The member function definition line to parse.
     * @return MethodInfo containing the parsed method information.
     */
    MethodInfo parseMethodDefinition(const std::string& line) const;

    /**
     * @brief Parses an include directive and extracts dependency information.
     * 
     * @param line The include directive line to parse.
     * @return DependencyInfo containing the parsed dependency information.
     */
    DependencyInfo parseIncludeDirective(const std::string& line) const;

    /**
     * @brief Extracts the module name from a CMake target.
     * 
     * @param targetName The CMake target name.
     * @return The extracted module name.
     */
    std::string extractModuleName(const std::string& targetName) const;

    /**
     * @brief Normalizes a path by applying prefix and suffix.
     * 
     * @param path The path to normalize.
     * @param prefix The prefix to apply.
     * @param suffix The suffix to apply.
     * @return The normalized path.
     */
    std::string normalizePath(const std::string& path,
                               const std::string& prefix,
                               const std::string& suffix) const;
};

} // namespace graph_lang_cpp
