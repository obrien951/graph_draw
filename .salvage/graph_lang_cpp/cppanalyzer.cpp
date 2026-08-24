#include "cppanalyzer.h"
#include <algorithm>
#include <filesystem>

namespace graph_lang_cpp {

const std::vector<std::string> CppAnalyzer::s_cppExtensions = {
    ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".hh", ".h++", ".inl", ".inc"
};

bool CppAnalyzer::detect(const RepoFileIndex& index) const {
    return hasCMakeLists(index) || hasCppSource(index);
}

bool CppAnalyzer::hasCMakeLists(const RepoFileIndex& index) const {
    return index.hasFile("CMakeLists.txt");
}

bool CppAnalyzer::hasCppSource(const RepoFileIndex& index) const {
    for (const auto& entry : index.files()) {
        const std::string& path = entry.path();
        for (const auto& ext : s_cppExtensions) {
            if (path.size() >= ext.size() && path.substr(path.size() - ext.size()) == ext) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> CppAnalyzer::cppExtensions() {
    return s_cppExtensions;
}

} // namespace graph_lang_cpp

