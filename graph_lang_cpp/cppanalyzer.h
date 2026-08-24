#pragma once
#include "languageanalyzer.h"

// The existing C++ scanner, moved behind LanguageAnalyzer (B3).
//
// This is PURE MOTION. isCppSource, isGeneratedSource, isNonFunctionWord,
// extractMethods, baseIdentifier, readCommandArgs, the class/include regexes and
// the CMake target parsing all arrive verbatim from graph_analyze/repoanalyzer.cpp.
// tests/test_repoanalyzer.cpp is the regression contract and must pass unedited.
//
// ONE deliberate behavioural change: the "uses" computation moves analyzer-side
// and iterates in sorted order. The original iterated QHash keys, so the edge
// array came out in hash order — stable within a run, arbitrary between builds.
class CppAnalyzer : public LanguageAnalyzer {
public:
    QString id() const override;

    // True when the index holds a CMakeLists.txt or any C/C++ source.
    bool detect(const RepoFileIndex& index) const override;

    // CMake targets -> modules, class/struct definitions -> types, member
    // functions -> methods, includes -> "uses". Appends; never clears.
    bool analyze(const RepoFileIndex& index, ir::Repo* repo,
                 QString* error) const override;
};
