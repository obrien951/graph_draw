#include "ir.h"
#include <algorithm>
#include <cctype>

namespace graph_lang {

void RepoFileIndex::addFile(const FileEntry& entry) {
    files_.push_back(entry);
}

bool RepoFileIndex::hasFile(const std::string& path) const {
    for (const auto& entry : files_) {
        if (entry.path == path) {
            return true;
        }
    }
    return false;
}

const FileEntry* RepoFileIndex::findFile(const std::string& path) const {
    for (const auto& entry : files_) {
        if (entry.path == path) {
            return &entry;
        }
    }
    return nullptr;
}

CFamilyAnalyzer::CFamilyAnalyzer() {}

std::string CFamilyAnalyzer::analyze(const FileEntry& entry) const {
    // C-family lexing and parsing would go here
    // For now, return a placeholder IR
    return "C-family IR for " + entry.basename;
}

PythonAnalyzer::PythonAnalyzer() {}

std::string PythonAnalyzer::analyze(const FileEntry& entry) const {
    // Python lexing and parsing would go here
    return "Python IR for " + entry.basename;
}

JSAnalyzer::JSAnalyzer() {}

std::string JSAnalyzer::analyze(const FileEntry& entry) const {
    // JS/TS lexing and parsing would go here
    return "JavaScript IR for " + entry.basename;
}

} // namespace graph_lang
