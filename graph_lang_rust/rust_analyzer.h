#pragma once

#include "languageanalyzer.h"

class TomlDocument;

class RustAnalyzer : public LanguageAnalyzer {
public:
    QString id() const override;
    bool detect(const RepoFileIndex& index) const override;
    bool analyze(const RepoFileIndex& index, ir::Repo* repo, QString* error) const override;
};
