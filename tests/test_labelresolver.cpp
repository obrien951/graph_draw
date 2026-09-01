#include <catch2/catch_test_macros.hpp>

#include "labelresolver.h"

#include <QString>
#include <QStringList>
#include <QVector>

using graph_lang::LabelResolver;

// ── Helpers ───────────────────────────────────────────────────────────────────

static LabelResolver::Item item(const QString& name, const QStringList& candidates)
{
    return LabelResolver::Item{ name, candidates };
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("A bare name no other item claims resolves to the bare name",
          "[labelresolver]")
{
    LabelResolver resolver;
    resolver.observe({
        item("AppState", { "AppState", "shared::AppState" }),
        item("Config",   { "Config",   "shared::Config" }),
    });

    REQUIRE(resolver.resolve(item("AppState", { "AppState", "shared::AppState" }))
            == "AppState");
    REQUIRE(resolver.resolve(item("Config", { "Config", "shared::Config" }))
            == "Config");
}

TEST_CASE("Colliding bare names escalate to the first unique qualifier",
          "[labelresolver]")
{
    LabelResolver resolver;
    const QVector<LabelResolver::Item> items = {
        item("Error",    { "Error",          "shared::Error",     "shared::lib::Error" }),
        item("Error",    { "Error",          "backend::Error",    "backend::lib::Error" }),
        item("Config",   { "Config",         "shared::Config" }),
        item("AppState", { "AppState",       "shared::AppState" }),
    };
    resolver.observe(items);

    REQUIRE(resolver.resolve(items[0]) == "shared::Error");
    REQUIRE(resolver.resolve(items[1]) == "backend::Error");
    REQUIRE(resolver.resolve(items[2]) == "Config");
    REQUIRE(resolver.resolve(items[3]) == "AppState");
}

TEST_CASE("Each colliding item gets its own first unique label",
          "[labelresolver]")
{
    LabelResolver resolver;
    const QVector<LabelResolver::Item> items = {
        item("Error", { "Error", "a::Error", "a::b::Error" }),
        item("Error", { "Error", "b::Error", "a::b::Error" }),
        item("Error", { "Error", "a::b::Error" }),
    };
    resolver.observe(items);

    REQUIRE(resolver.resolve(items[0]) == "a::Error");
    REQUIRE(resolver.resolve(items[1]) == "b::Error");
    REQUIRE(resolver.resolve(items[2]) == "a::b::Error");
}

TEST_CASE("An item whose candidates are all contested resolves to the longest",
          "[labelresolver]")
{
    LabelResolver resolver;
    const QStringList candidates{ "Foo", "a::Foo", "a::b::Foo" };
    resolver.observe({
        item("Foo", candidates),
        item("Foo", candidates),
    });

    REQUIRE(resolver.resolve(item("Foo", candidates)) == "a::b::Foo");
}

TEST_CASE("A candidate repeated inside one item's list claims the label once",
          "[labelresolver]")
{
    LabelResolver resolver;
    resolver.observe({
        item("Foo", { "Foo", "Foo", "a::Foo" }),
    });

    REQUIRE(resolver.resolve(item("Foo", { "Foo", "Foo", "a::Foo" })) == "Foo");
}

TEST_CASE("An item with no candidates falls back to its bare name",
          "[labelresolver]")
{
    LabelResolver resolver;
    resolver.observe({ item("Widget", {}) });

    REQUIRE(resolver.resolve(item("Widget", {})) == "Widget");
}

TEST_CASE("resolve() before observe() finds no conflicts and picks the first candidate",
          "[labelresolver]")
{
    LabelResolver resolver;

    REQUIRE(resolver.resolve(item("Foo", { "Foo", "a::Foo" })) == "Foo");
}

TEST_CASE("The first candidate unique repo-wide wins, not the shortest",
          "[labelresolver]")
{
    // The spec promises "the first candidate that is unique repo-wide":
    // an earlier unambiguous candidate beats a later, shorter one.
    LabelResolver resolver;
    resolver.observe({
        item("Error", { "Error", "shared::Error", "shared::lib::Error" }),
        item("Error", { "backend::lib::Error", "backend::Error", "Error" }),
    });

    REQUIRE(resolver.resolve(item("Error", { "Error", "shared::Error", "shared::lib::Error" }))
            == "shared::Error");
    REQUIRE(resolver.resolve(item("Error", { "backend::lib::Error", "backend::Error", "Error" }))
            == "backend::lib::Error");
}

TEST_CASE("observe() with an empty item list is a no-op",
          "[labelresolver]")
{
    LabelResolver resolver;
    resolver.observe({});

    // No claims were registered, so resolution behaves as if observe() was
    // never called: the first candidate is unambiguous.
    REQUIRE(resolver.resolve(item("Foo", { "Foo", "a::Foo" })) == "Foo");
    REQUIRE(resolver.resolve(item("Widget", {})) == "Widget");
}

TEST_CASE("observe() counts claims per item across the whole collection",
          "[labelresolver]")
{
    LabelResolver resolver;
    const QVector<LabelResolver::Item> items = {
        item("Alpha",  { "Alpha",  "shared::Alpha" }),
        item("Beta",   { "Beta",   "shared::Beta"  }),
        item("Gamma",  { "Gamma",  "shared::Gamma" }),
        item("Alpha2", { "Alpha2", "shared::Alpha" }),  // also claims shared::Alpha
    };
    resolver.observe(items);

    // shared::Alpha is claimed by two items, so it is contested for both
    // "Alpha" and "Alpha2"; each falls back to its own (uncontested) bare name.
    REQUIRE(resolver.resolve(items[0]) == "Alpha");
    REQUIRE(resolver.resolve(items[1]) == "Beta");
    REQUIRE(resolver.resolve(items[2]) == "Gamma");
    REQUIRE(resolver.resolve(items[3]) == "Alpha2");
}

TEST_CASE("Empty-string candidates are ignored and never chosen as labels",
          "[labelresolver]")
{
    // "" is not a usable label: observe() must not count it as a claim and
    // resolve() must not return it, falling back to the bare name when no
    // real candidates remain.
    LabelResolver resolver;
    resolver.observe({
        item("Widget", { "", "app::Widget" }),
        item("Gadget", { "" }),
    });

    REQUIRE(resolver.resolve(item("Widget", { "", "app::Widget" })) == "app::Widget");
    REQUIRE(resolver.resolve(item("Gadget", { "" })) == "Gadget");
}

TEST_CASE("An empty candidate claimed by many items is ignored, not contested",
          "[labelresolver]")
{
    // Without filtering, "" would be claimed by both items and resolve()
    // could treat it as a contested label. It must simply be ignored.
    LabelResolver resolver;
    resolver.observe({
        item("Foo", { "", "Foo", "a::Foo" }),
        item("Bar", { "", "Bar", "b::Bar" }),
    });

    REQUIRE(resolver.resolve(item("Foo", { "", "Foo", "a::Foo" })) == "Foo");
    REQUIRE(resolver.resolve(item("Bar", { "", "Bar", "b::Bar" })) == "Bar");
}
