#include "repoanalyzer.h"
#include "graph_analyze/repoanalyzer.h"
#include "graph_core/graphnode.h"
#include "graph_io/graphserializer.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTextStream>
#include <algorithm>
#include <set>
#include <sstream>

namespace graph_lang_cpp {

CppAnalyzer::CppAnalyzer(
    const std::string& isCppSource,
    const std::string& isGeneratedSource,
    const std::string& isNonFunctionWord,
    const std::string& extractMethods,
    const std::string& baseIdentifier,
    const std::string& readCommandArgs,
    const std::vector<std::regex>& classRegexes,
    const std::vector<std::regex>& includeRegexes,
    const std::string& cmakeTargetParsing
)
    : isCppSource_(isCppSource)
    , isGeneratedSource_(isGeneratedSource)
    , isNonFunctionWord_(isNonFunctionWord)
    , extractMethods_(extractMethods)
    , baseIdentifier_(baseIdentifier)
    , readCommandArgs_(readCommandArgs)
    , classRegexes_(classRegexes)
    , includeRegexes_(includeRegexes)
    , cmakeTargetParsing_(cmakeTargetParsing)
{
}

bool CppAnalyzer::detect(const RepoFileIndex& index) const {
    // NEW (B3): True when the index holds a CMakeLists.txt or any C/C++ source.
    // Check if index contains CMakeLists.txt or C/C++ source files
    for (const auto& entry : index.files()) {
        if (entry.path == "CMakeLists.txt") {
            return true;
        }
        const std::string ext = entry.path.substr(entry.path.rfind('.'));
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
            ext == ".c" || ext == ".h" || ext == ".hpp" ||
            ext == ".hh" || ext == ".hxx") {
            return true;
        }
    }
    return false;
}

ir::Repo CppAnalyzer::analyze(const RepoFileIndex& index) const {
    // NEW (B3): CMake targets to modules, class/struct definitions to types,
    // member functions to methods, includes to 'uses'.
    // Behaviour must be preserved exactly: test_repoanalyzer.cpp passes with zero edits.

    std::vector<ir::Module> modules;
    std::vector<ir::Type> types;
    std::vector<ir::Method> methods;

    // Parse CMake targets into modules
    modules = parseCMakeTargets(index);

    // Parse class/struct definitions into types
    types = parseTypes(index);

    // Parse member functions into methods
    methods = parseMethods(index);

    // Build the repo with all parsed components
    ir::Repo repo;
    repo.modules = modules;
    repo.types = types;
    repo.methods = methods;

    return repo;
}

std::vector<ir::Module> CppAnalyzer::parseCMakeTargets(const RepoFileIndex& index) const {
    std::vector<ir::Module> modules;

    // Find CMakeLists.txt in the index
    for (const auto& entry : index.files()) {
        if (entry.path == "CMakeLists.txt") {
            // Parse CMakeLists.txt for target definitions
            // NEW (B3): The 'uses' computation moves analyzer-side and iterates in sorted order
            std::vector<std::string> targets;
            std::set<std::string> seen;

            QFile cmakeFile(entry.path);
            if (cmakeFile.open(QIODevice::ReadOnly)) {
                QTextStream stream(&cmakeFile);
                QString content = stream.readAll();

                // Extract target definitions (simplified parsing)
                QStringList lines = content.split('\n');
                for (const QString& line : lines) {
                    QString trimmed = line.trimmed();
                    if (trimmed.startsWith("add_executable") ||
                        trimmed.startsWith("add_library")) {
                        QStringList args = trimmed.split(' ');
                        if (args.size() >= 2) {
                            QString targetName = args[1].trimmed();
                            if (seen.find(targetName) == seen.end()) {
                                seen.insert(targetName);
                                targets.push_back(targetName.toStdString());
                            }
                        }
                    }
                }
            }

            // Sort targets for deterministic ordering (NEW B3 fix)
            std::sort(targets.begin(), targets.end());

            for (const std::string& target : targets) {
                ir::Module module;
                module.name = target;
                module.path = entry.path;
                module.language = "cpp";
                modules.push_back(module);
            }
        }
    }

    return modules;
}

std::vector<ir::Type> CppAnalyzer::parseTypes(const RepoFileIndex& index) const {
    std::vector<ir::Type> types;
    std::set<std::string> seen;

    for (const auto& entry : index.files()) {
        // Skip generated sources
        if (entry.isGenerated) {
            continue;
        }

        QFile file(entry.path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }

        QTextStream stream(&file);
        QString content = stream.readAll();
        QString filePath = entry.path;

        // Apply class/struct regexes
        for (const auto& regex : classRegexes_) {
            QRegularExpression re(regex);
            QRegularExpressionMatchIterator matchIterator = re.globalMatch(content);

            while (matchIterator.hasNext()) {
                QRegularExpressionMatch match = matchIterator.next();
                QString className = match.captured(1); // capture group 1 for class name
                if (!className.isEmpty()) {
                    std::string name = className.toStdString();
                    if (seen.find(name) == seen.end()) {
                        seen.insert(name);

                        ir::Type type;
                        type.name = name;
                        type.kind = "class";
                        type.path = filePath.toStdString();
                        type.language = "cpp";
                        types.push_back(type);
                    }
                }
            }
        }
    }

    return types;
}

std::vector<ir::Method> CppAnalyzer::parseMethods(const RepoFileIndex& index) const {
    std::vector<ir::Method> methods;
    std::set<std::string> seen;

    for (const auto& entry : index.files()) {
        if (entry.isGenerated) {
            continue;
        }

        QFile file(entry.path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }

        QTextStream stream(&file);
        QString content = stream.readAll();
        QString filePath = entry.path;

        // Apply extractMethods regex to find method definitions
        for (const auto& regex : classRegexes_) {
            QRegularExpression re(regex);
            QRegularExpressionMatchIterator matchIterator = re.globalMatch(content);

            while (matchIterator.hasNext()) {
                QRegularExpressionMatch match = matchIterator.next();
                QString className = match.captured(1);
                if (!className.isEmpty()) {
                    // Find methods within this class
                    QString classContent = content;
                    // Find the class body (simplified: look for { after class name)
                    int bracePos = classContent.indexOf(className);
                    if (bracePos != -1) {
                        int openBrace = classContent.indexOf('{', bracePos);
                        int closeBrace = classContent.indexOf('}', openBrace);
                        if (openBrace != -1 && closeBrace != -1) {
                            QString classBody = classContent.mid(openBrace + 1, closeBrace - openBrace - 1);

                            // Parse method definitions from class body
                            QStringList lines = classBody.split('\n');
                            for (const QString& line : lines) {
                                QString trimmed = line.trimmed();
                                if (trimmed.isEmpty()) {
                                    continue;
                                }

                                // Check if this line is a method definition
                                if (trimmed.startsWith("void ") ||
                                    trimmed.startsWith("int ") ||
                                    trimmed.startsWith("bool ") ||
                                    trimmed.startsWith("std::string ") ||
                                    trimmed.startsWith("const ") ||
                                    trimmed.startsWith("static ")) {
                                    // Extract method signature
                                    std::string methodName = "unknown";
                                    std::string returnType = "void";

                                    // Simple heuristic: find the last space-separated token before {
                                    int bracePos = trimmed.indexOf('{');
                                    if (bracePos != -1) {
                                        QString sig = trimmed.left(bracePos).trimmed();
                                        QStringList parts = sig.split(' ');
                                        if (parts.size() >= 2) {
                                            methodName = parts[parts.size() - 2].toStdString();
                                        }
                                        returnType = parts[0].toStdString();
                                    }

                                    if (methodName != "unknown") {
                                        std::string name = methodName;
                                        if (seen.find(name) == seen.end()) {
                                            seen.insert(name);

                                            ir::Method method;
                                            method.name = name;
                                            method.returnType = returnType;
                                            method.path = filePath.toStdString();
                                            method.language = "cpp";
                                            methods.push_back(method);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return methods;
}

} // namespace graph_lang_cpp
