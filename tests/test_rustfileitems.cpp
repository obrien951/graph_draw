#include <catch2/catch_test_macros.hpp>

#include "rust_file_items.h"

// Unit tests for scanRustFile itself, independent of RustAnalyzer. The
// previous implementation classified an item by the text of the line its
// CLOSING brace landed on rather than the line that opened it — which only
// ever matched single-line items, i.e. almost nothing in real Rust code.
// These pin down the rewritten, multi-line-correct behaviour.

namespace {

RustFileItems::Type typeNamed(const RustFileItems& items, const QString& label)
{
    for (const auto& t : items.types)
        if (t.label == label) return t;
    return RustFileItems::Type{};
}

bool hasFunctionNamed(const QVector<RustFileItems::Item>& fns, const QString& label)
{
    for (const auto& f : fns)
        if (f.label == label) return true;
    return false;
}

} // namespace

TEST_CASE("scanRustFile finds a struct whose body spans multiple lines", "[rustfileitems]")
{
    const QString src =
        "pub struct User {\n"
        "    id: u64,\n"
        "    name: String,\n"
        "}\n";
    const RustFileItems items = scanRustFile(src);
    REQUIRE(items.types.size() == 1);
    CHECK(items.types[0].label == QStringLiteral("User"));
}

TEST_CASE("scanRustFile attaches a trait's required and provided methods", "[rustfileitems]")
{
    const QString src =
        "pub trait Repository {\n"
        "    fn find(&self, id: u64) -> Option<String>;\n"
        "\n"
        "    fn find_or_default(&self, id: u64) -> String {\n"
        "        self.find(id).unwrap_or_default()\n"
        "    }\n"
        "}\n";
    const RustFileItems items = scanRustFile(src);
    const RustFileItems::Type trait = typeNamed(items, QStringLiteral("Repository"));
    REQUIRE(trait.label == QStringLiteral("Repository"));

    QStringList methodNames;
    for (const auto& m : trait.methods) methodNames.append(m.label);
    CHECK(methodNames.contains(QStringLiteral("find")));
    CHECK(methodNames.contains(QStringLiteral("find_or_default")));
    // Only the trait's own two methods — nothing from inside find_or_default's body.
    CHECK(trait.methods.size() == 2);
}

TEST_CASE("scanRustFile records the trait an impl targets and its methods", "[rustfileitems]")
{
    const QString src =
        "pub struct PgUsers;\n"
        "\n"
        "impl Repository for PgUsers {\n"
        "    fn find(&self, id: u64) -> Option<String> {\n"
        "        None\n"
        "    }\n"
        "}\n";
    const RustFileItems items = scanRustFile(src);
    const RustFileItems::Type pgUsers = typeNamed(items, QStringLiteral("PgUsers"));
    REQUIRE(pgUsers.label == QStringLiteral("PgUsers"));
    REQUIRE(pgUsers.implementsTraits.size() == 1);
    CHECK(pgUsers.implementsTraits.first() == QStringLiteral("Repository"));
    REQUIRE(pgUsers.methods.size() == 1);
    CHECK(pgUsers.methods.first().label == QStringLiteral("find"));
}

TEST_CASE("scanRustFile handles generic impls with a where clause", "[rustfileitems]")
{
    const QString src =
        "struct Cache<T> { value: T }\n"
        "\n"
        "impl<T> Repository for Cache<T> where T: Clone {\n"
        "    fn find(&self, id: u64) -> Option<String> {\n"
        "        None\n"
        "    }\n"
        "}\n";
    const RustFileItems items = scanRustFile(src);
    const RustFileItems::Type cache = typeNamed(items, QStringLiteral("Cache"));
    REQUIRE(cache.label == QStringLiteral("Cache"));
    REQUIRE(cache.implementsTraits.size() == 1);
    CHECK(cache.implementsTraits.first() == QStringLiteral("Repository"));
}

TEST_CASE("scanRustFile reports the struct's own declaration line, not the derive or the doc",
          "[rustfileitems]")
{
    const QString src =
        "/// A user account.\n"
        "#[derive(Debug, Serialize)]\n"
        "pub struct User {\n"
        "    id: u64,\n"
        "}\n";
    const RustFileItems items = scanRustFile(src);
    REQUIRE(items.types.size() == 1);
    // Line 0: doc, line 1: derive, line 2 (0-based): "pub struct User {".
    CHECK(items.types[0].line == 2);
}

TEST_CASE("scanRustFile does not surface a fn nested inside another fn's body", "[rustfileitems]")
{
    const QString src =
        "fn outer() {\n"
        "    fn inner() {\n"
        "        let _ = 1;\n"
        "    }\n"
        "    inner();\n"
        "}\n";
    const RustFileItems items = scanRustFile(src);
    REQUIRE(items.functions.size() == 1);
    CHECK(items.functions.first().label == QStringLiteral("outer"));
    CHECK_FALSE(hasFunctionNamed(items.functions, QStringLiteral("inner")));
}

TEST_CASE("scanRustFile does not surface a fn nested inside a block expression", "[rustfileitems]")
{
    const QString src =
        "fn outer() {\n"
        "    if true {\n"
        "        fn inner() {}\n"
        "    }\n"
        "}\n";
    const RustFileItems items = scanRustFile(src);
    REQUIRE(items.functions.size() == 1);
    CHECK(items.functions.first().label == QStringLiteral("outer"));
}

TEST_CASE("scanRustFile recognizes tuple and unit structs with no braces", "[rustfileitems]")
{
    const QString src =
        "struct Point(i32, i32);\n"
        "struct Unit;\n";
    const RustFileItems items = scanRustFile(src);
    CHECK(typeNamed(items, QStringLiteral("Point")).label == QStringLiteral("Point"));
    CHECK(typeNamed(items, QStringLiteral("Unit")).label == QStringLiteral("Unit"));
}

TEST_CASE("scanRustFile expands use groups, aliases, and self into leaf names", "[rustfileitems]")
{
    const QString src =
        "use std::io::{self, Write};\n"
        "use crate::models::User as Account;\n"
        "use crate::prelude::*;\n"
        "\n"
        "fn noop() {}\n";
    const RustFileItems items = scanRustFile(src);
    CHECK(items.usePaths.contains(QStringLiteral("Write")));
    CHECK(items.usePaths.contains(QStringLiteral("User"))); // pre-alias name, not "Account"
    CHECK_FALSE(items.usePaths.contains(QStringLiteral("Account")));
}

TEST_CASE("scanRustFile collects leading //! module doc lines", "[rustfileitems]")
{
    const QString src =
        "//! Request routing for the HTTP API.\n"
        "//! Nothing in here touches the database directly.\n"
        "\n"
        "fn noop() {}\n";
    const RustFileItems items = scanRustFile(src);
    CHECK(items.innerDoc == QStringLiteral(
        "Request routing for the HTTP API. Nothing in here touches the database directly."));
}
