#include <catch2/catch_test_macros.hpp>

#include "rust_analyzer.h"

TEST_CASE("The Rust analyzer identifies itself as rust", "[rustanalyzer]")
{
    REQUIRE(RustAnalyzer().id() == QStringLiteral("rust"));
}
