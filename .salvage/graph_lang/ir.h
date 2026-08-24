#ifndef GRAPH_LANG_IR_H
#define GRAPH_LANG_IR_H

#include <string>
#include <vector>
#include <memory>

namespace graph_lang {

/**
 * @brief Intermediate representation of a single file in the repository.
 *
 * Represents a file with its path, basename, and parsed content.
 */
struct FileEntry {
    std::string path;          // Full path relative to repo root
    std::string basename;      // Just the filename
    std::string content;       // Raw file content
    std::string language;      // Detected or declared language
};

/**
 * @brief Index of all files in the repository, sorted deterministically.
 *
 * Files are sorted first by directory depth, then lexicographically
 * within each directory level. This ensures deterministic ordering
 * across runs and builds.
 */
class RepoFileIndex {
public:
    RepoFileIndex() = default;

    /**
     * @brief Add a single file entry to the index.
     */
    void addFile(const FileEntry& entry);

    /**
     * @brief Get all files in sorted order.
     *
     * Sorting is by directory depth (ascending), then lexicographically
     * by path within each depth level.
     */
    const std::vector<FileEntry>& files() const { return files_; }

    /**
     * @brief Get the number of files in the index.
     */
    size_t size() const { return files_.size(); }

    /**
     * @brief Check if a file exists in the index.
     */
    bool hasFile(const std::string& path) const;

    /**
     * @brief Get a file entry by path.
     */
    const FileEntry* findFile(const std::string& path) const;

private:
    std::vector<FileEntry> files_;
};

/**
 * @brief A language analyzer that produces IR from source files.
 *
 * This is the interface that all language-specific analyzers must implement.
 * The graph_lang module is language-neutral and delegates to these analyzers.
 */
class LanguageAnalyzer {
public:
    virtual ~LanguageAnalyzer() = default;

    /**
     * @brief Analyze a single file and produce IR.
     *
     * @param entry The file entry from the repo index.
     * @return The IR representation of the file.
     */
    virtual std::string analyze(const FileEntry& entry) const = 0;

    /**
     * @brief Get the language name for this analyzer.
     */
    virtual std::string languageName() const = 0;
};

/**
 * @brief A language analyzer that produces IR from C-family source files.
 *
 * Uses C-family lexing helpers to tokenize and parse C/C++/Objective-C
 * source files into an intermediate representation.
 */
class CFamilyAnalyzer : public LanguageAnalyzer {
public:
    CFamilyAnalyzer();

    std::string analyze(const FileEntry& entry) const override;
    std::string languageName() const override { return "C-family"; }
};

/**
 * @brief A language analyzer that produces IR from Python source files.
 */
class PythonAnalyzer : public LanguageAnalyzer {
public:
    PythonAnalyzer();

    std::string analyze(const FileEntry& entry) const override;
    std::string languageName() const override { return "Python"; }
};

/**
 * @brief A language analyzer that produces IR from JavaScript/TypeScript files.
 */
class JSAnalyzer : public LanguageAnalyzer {
public:
    JSAnalyzer();

    std::string analyze(const FileEntry& entry) const override;
    std::string languageName() const override { return "JavaScript"; }
};

} // namespace graph_lang

#endif // GRAPH_LANG_IR_H
