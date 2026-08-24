#pragma once

#include <string>
#include <vector>
#include <memory>
#include <regex>
#include <QRegularExpression>
#include "graph_core/graphnode.h"
#include "graph_io/graphserializer.h"

namespace graph_lang_cpp {

class RepoFileIndex;
class ir::Repo;

class CppAnalyzer {
public:
    CppAnalyzer(
        const std::string& isCppSource,
        const std::string& isGeneratedSource,
        const std::string& isNonFunctionWord,
        const std::string& extractMethods,
        const std::string& baseIdentifier,
        const std::string& readCommandArgs,
        const std::vector<std::regex>& classRegexes,
        const std::vector<std::regex>& includeRegexes,
        const std::string& cmakeTargetParsing
    );

    bool detect(const RepoFileIndex& index) const;
    ir::Repo analyze(const RepoFileIndex& index) const;

private:
    std::string isCppSource_;
    std::string isGeneratedSource_;
    std::string isNonFunctionWord_;
    std::string extractMethods_;
    std::string baseIdentifier_;
    std::string readCommandArgs_;
    std::vector<std::regex> classRegexes_;
    std::vector<std::regex> includeRegexes_;
    std::string cmakeTargetParsing_;

    std::vector<ir::Module> parseModules(const RepoFileIndex& index) const;
    std::vector<ir::Type> parseTypes(const RepoFileIndex& index) const;
    std::vector<ir::Method> parseMethods(const RepoFileIndex& index) const;
    std::vector<ir::Module> parseCMakeTargets(const RepoFileIndex& index) const;
};

} // namespace graph_lang_cpp
