#pragma once
#include "rust_file_items.h"
#include <QString>

// scanRustFile: Single brace-depth scanner over blanked text.
// Items are recognised only at item depth (depth == 0), so fns inside function
// bodies and impls inside macro_rules! bodies never reach the graph.
// impl headers need a real scan rather than regex because of generics and where clauses.
RustFileItems scanRustFile(const QString& filePath, const QString& blankedText);
