#include <catch2/catch_test_macros.hpp>

#include "rust_analyzer.h"
#include "repofileindex.h"
#include "ir.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

TEST_CASE("The Rust analyzer identifies itself as rust", "[rustanalyzer]")
{
    REQUIRE(RustAnalyzer().id() == QStringLiteral("rust"));
}

namespace {

void writeFile(const QString& path, const QString& contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(contents.toUtf8());
    f.close();
}

// Two crates: "backend" (with a nested src/routes.rs module and a src/db/
// directory module whose own contents must not surface individually)
// depending on "shared_utils" via the hyphenated spelling "shared-utils" —
// exercising '-'/'_' normalization — plus a decoy target/ that must
// contribute nothing.
void buildSampleCargoWorkspace(const QDir& root)
{
    writeFile(root.filePath("Cargo.toml"),
              "[workspace]\n"
              "members = [\"backend\", \"shared-utils\"]\n"
              "\n"
              "[workspace.dependencies]\n"
              "serde = \"1.0\"\n");

    writeFile(root.filePath("backend/Cargo.toml"),
              "[package]\n"
              "name = \"backend\"\n"
              "version = \"0.1.0\"\n"
              "\n"
              "[dependencies]\n"
              "shared-utils = \"0.1\"\n"
              "serde = \"1.0\"\n");
    writeFile(root.filePath("backend/src/main.rs"), "fn main() {}\n");
    writeFile(root.filePath("backend/src/routes.rs"),
              "pub fn handler() {\n"
              "    fn inner_helper() {}\n"
              "    inner_helper();\n"
              "}\n");
    writeFile(root.filePath("backend/src/db/mod.rs"), "pub mod queries;\n");
    writeFile(root.filePath("backend/src/db/queries.rs"), "pub fn all() {}\n");

    writeFile(root.filePath("backend/src/models.rs"),
              "/// A user account.\n"
              "#[derive(Debug, Serialize)]\n"
              "pub struct User {\n"
              "    id: u64,\n"
              "}\n"
              "\n"
              "impl User {\n"
              "    pub fn new(id: u64) -> Self {\n"
              "        User { id }\n"
              "    }\n"
              "}\n");
    writeFile(root.filePath("backend/src/repository.rs"),
              "use crate::models::User;\n"
              "\n"
              "pub trait Repository {\n"
              "    fn find(&self, id: u64) -> Option<User>;\n"
              "\n"
              "    fn find_or_default(&self, id: u64) -> User {\n"
              "        self.find(id).unwrap_or(User::new(0))\n"
              "    }\n"
              "}\n"
              "\n"
              "pub struct PgUsers;\n"
              "\n"
              "impl Repository for PgUsers {\n"
              "    fn find(&self, id: u64) -> Option<User> {\n"
              "        None\n"
              "    }\n"
              "}\n");

    writeFile(root.filePath("shared-utils/Cargo.toml"),
              "[package]\n"
              "name = \"shared_utils\"\n"
              "version = \"0.1.0\"\n");
    writeFile(root.filePath("shared-utils/src/lib.rs"), "pub fn helper() {}\n");

    writeFile(root.filePath("target/CACHEDIR.TAG"),
              "Signature: 8a477f597d28d172789f06886806bc55\n");
    writeFile(root.filePath("target/decoy.rs"), "fn should_not_appear() {}\n");
}

} // namespace

TEST_CASE("RustAnalyzer::detect recognizes a Cargo workspace", "[rustanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleCargoWorkspace(root);

    const RepoFileIndex index = buildRepoFileIndex(root.absolutePath());
    REQUIRE(RustAnalyzer().detect(index));
}

TEST_CASE("RustAnalyzer::analyze builds crate modules, depth-1 nested modules, "
          "and normalized cross-crate dependency edges", "[rustanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleCargoWorkspace(root);

    const RepoFileIndex index = buildRepoFileIndex(root.absolutePath());
    ir::Repo repo;
    QString error;
    REQUIRE(RustAnalyzer().analyze(index, &repo, &error));

    REQUIRE(repo.hasModule(QStringLiteral("backend")));
    REQUIRE(repo.hasModule(QStringLiteral("shared_utils")));

    const ir::Module* backend = repo.module(QStringLiteral("backend"));
    REQUIRE(backend != nullptr);
    CHECK(backend->childModuleLabels.contains(QStringLiteral("routes")));
    CHECK(backend->childModuleLabels.contains(QStringLiteral("db")));
    CHECK_FALSE(backend->childModuleLabels.contains(QStringLiteral("main")));
    CHECK_FALSE(backend->childModuleLabels.contains(QStringLiteral("queries")));

    const ir::Module* sharedUtils = repo.module(QStringLiteral("shared_utils"));
    REQUIRE(sharedUtils != nullptr);
    CHECK(sharedUtils->childModuleLabels.isEmpty());

    // The dependency was declared as "shared-utils" (hyphenated); the edge
    // must resolve to the module's real label, "shared_utils".
    CHECK(backend->dependsOnLabels.contains(QStringLiteral("shared_utils")));
    CHECK_FALSE(backend->dependsOnLabels.contains(QStringLiteral("serde")));

    CHECK_FALSE(repo.moduleLabels().contains(QStringLiteral("queries")));
    CHECK_FALSE(repo.moduleLabels().contains(QStringLiteral("decoy")));
}

namespace {

bool hasFreeFunction(const ir::Repo& repo, const QString& label)
{
    for (const ir::Module& m : repo.modules())
        for (const ir::Function& f : m.freeFunctions)
            if (f.label == label) return true;
    return false;
}

bool hasMethod(const ir::Type& t, const QString& qualifiedLabel)
{
    for (const ir::Function& f : t.methods)
        if (f.label == qualifiedLabel) return true;
    return false;
}

} // namespace

TEST_CASE("RustAnalyzer::analyze extracts types, methods, inherits and uses edges "
          "from Rust source, and never surfaces a fn nested in another fn's body",
          "[rustanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildSampleCargoWorkspace(root);

    const RepoFileIndex index = buildRepoFileIndex(root.absolutePath());
    ir::Repo repo;
    QString error;
    REQUIRE(RustAnalyzer().analyze(index, &repo, &error));

    // A doc comment survives a #[derive(...)] sitting between it and the
    // struct — the attribute must not be mistaken for the item itself.
    const ir::Type* user = repo.type(QStringLiteral("User"));
    REQUIRE(user != nullptr);
    CHECK(user->description == QStringLiteral("A user account."));
    CHECK(user->moduleLabel == QStringLiteral("models"));
    CHECK(hasMethod(*user, QStringLiteral("User::new")));

    // A required trait method (no body) and a provided one (default body)
    // both surface as methods on the trait itself.
    const ir::Type* repository = repo.type(QStringLiteral("Repository"));
    REQUIRE(repository != nullptr);
    CHECK(hasMethod(*repository, QStringLiteral("Repository::find")));
    CHECK(hasMethod(*repository, QStringLiteral("Repository::find_or_default")));

    // "impl Repository for PgUsers" -> an inherits edge from PgUsers to
    // Repository, plus PgUsers's own method from that impl block.
    const ir::Type* pgUsers = repo.type(QStringLiteral("PgUsers"));
    REQUIRE(pgUsers != nullptr);
    CHECK(pgUsers->baseLabels.contains(QStringLiteral("Repository")));
    CHECK(hasMethod(*pgUsers, QStringLiteral("PgUsers::find")));

    // repository.rs's `use crate::models::User;` resolves against the
    // repo-wide type set; every type that file declares or implements (the
    // trait itself, and PgUsers via its impl block) is considered to use it.
    CHECK(pgUsers->usesLabels.contains(QStringLiteral("User")));
    CHECK(repository->usesLabels.contains(QStringLiteral("User")));

    // routes.rs's handler() contains a nested fn — it must never appear as
    // a free function or a method anywhere in the graph.
    CHECK(hasFreeFunction(repo, QStringLiteral("handler")));
    CHECK_FALSE(hasFreeFunction(repo, QStringLiteral("inner_helper")));
    for (const ir::Type& t : repo.types())
        CHECK_FALSE(hasMethod(t, QStringLiteral("inner_helper")));
}

namespace {

// Two independent crates, each with their own `fn main()` and their own
// `struct Config`; crate_a additionally has a nested `routes` module with
// its own `fn connect()` distinct from crate_a's own root-level `connect()`.
// Before R4.5, the analyzer's own per-file merge indexed types and free
// functions by bare label alone (not scoped to the owning module), so every
// one of these same-named pairs collapsed into a single IR entry long before
// GraphBuilder's own label resolution ever got a chance to run.
void buildCollidingNamesWorkspace(const QDir& root)
{
    writeFile(root.filePath("Cargo.toml"),
              "[workspace]\n"
              "members = [\"crate_a\", \"crate_b\"]\n");

    writeFile(root.filePath("crate_a/Cargo.toml"),
              "[package]\n"
              "name = \"crate_a\"\n"
              "version = \"0.1.0\"\n");
    // Config lives directly in main.rs (crate-root scope, module label
    // "crate_a"/"crate_b") rather than in its own same-named file here —
    // this test is specifically about collisions at crate-root scope and
    // within one crate's own tree (root vs. nested module). The harder case
    // of two DIFFERENT crates each having their own same-named *nested*
    // module (e.g. both with src/config.rs) is covered separately below by
    // "RustAnalyzer::analyze disambiguates nested modules of the same name
    // across crates" — it isn't out of scope anymore, it just needs
    // ir::Module::parentLabel/ir::Type::ownerCrate, which didn't exist yet
    // when this test was written.
    writeFile(root.filePath("crate_a/src/main.rs"),
              "fn connect() {}\n"
              "\n"
              "fn main() {\n"
              "    connect();\n"
              "}\n"
              "\n"
              "/// crate_a's own configuration.\n"
              "pub struct Config {\n"
              "    port: u16,\n"
              "}\n");
    writeFile(root.filePath("crate_a/src/routes.rs"), "fn connect() {}\n");

    writeFile(root.filePath("crate_b/Cargo.toml"),
              "[package]\n"
              "name = \"crate_b\"\n"
              "version = \"0.1.0\"\n");
    writeFile(root.filePath("crate_b/src/main.rs"),
              "fn main() {}\n"
              "\n"
              "/// crate_b's own configuration.\n"
              "pub struct Config {\n"
              "    retries: u8,\n"
              "}\n");
}

int countFreeFunctionsNamed(const ir::Repo& repo, const QString& owningModule,
                             const QString& label)
{
    int count = 0;
    for (const ir::Module& m : repo.modules()) {
        if (m.label != owningModule) continue;
        for (const ir::Function& f : m.freeFunctions)
            if (f.label == label) ++count;
    }
    return count;
}

} // namespace

TEST_CASE("RustAnalyzer::analyze keeps same-named types and free functions "
          "distinct across different crates, and across a crate and its own "
          "nested module (R4.5)", "[rustanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildCollidingNamesWorkspace(root);

    const RepoFileIndex index = buildRepoFileIndex(root.absolutePath());
    ir::Repo repo;
    QString error;
    REQUIRE(RustAnalyzer().analyze(index, &repo, &error));

    // Two distinct `main` functions, one correctly owned by each crate — not
    // one crate's main silently eating the other's.
    CHECK(countFreeFunctionsNamed(repo, QStringLiteral("crate_a"), QStringLiteral("main")) == 1);
    CHECK(countFreeFunctionsNamed(repo, QStringLiteral("crate_b"), QStringLiteral("main")) == 1);

    // Two distinct `connect` functions within crate_a alone: one owned by the
    // crate root, one by its nested `routes` module.
    CHECK(countFreeFunctionsNamed(repo, QStringLiteral("crate_a"), QStringLiteral("connect")) == 1);
    CHECK(countFreeFunctionsNamed(repo, QStringLiteral("routes"), QStringLiteral("connect")) == 1);

    // Two distinct `Config` types, each keeping its own doc and owning module
    // — not merged into one with only the first crate's doc surviving.
    int configCount = 0;
    bool sawCrateADoc = false, sawCrateBDoc = false;
    for (const ir::Type& t : repo.types()) {
        if (t.label != QStringLiteral("Config")) continue;
        ++configCount;
        if (t.moduleLabel == QStringLiteral("crate_a"))
            sawCrateADoc = t.description == QStringLiteral("crate_a's own configuration.");
        if (t.moduleLabel == QStringLiteral("crate_b"))
            sawCrateBDoc = t.description == QStringLiteral("crate_b's own configuration.");
    }
    CHECK(configCount == 2);
    CHECK(sawCrateADoc);
    CHECK(sawCrateBDoc);
}

namespace {

// The limitation R4.5's test above explicitly called out of scope: two
// crates each with their OWN nested module of the same bare name (both have
// src/config.rs), each in turn declaring a type and a free function that
// also share a bare name. ir::Module::parentLabel and ir::Type::ownerCrate
// exist specifically to let this disambiguate correctly instead of merging.
void buildNestedModuleCollisionWorkspace(const QDir& root)
{
    writeFile(root.filePath("Cargo.toml"),
              "[workspace]\n"
              "members = [\"crate_a\", \"crate_b\"]\n");

    writeFile(root.filePath("crate_a/Cargo.toml"),
              "[package]\n"
              "name = \"crate_a\"\n"
              "version = \"0.1.0\"\n");
    writeFile(root.filePath("crate_a/src/main.rs"), "mod config;\nfn main() {}\n");
    writeFile(root.filePath("crate_a/src/config.rs"),
              "/// A's settings.\n"
              "pub struct Settings;\n"
              "\n"
              "/// A's loader.\n"
              "pub fn load() -> Settings { Settings }\n");

    writeFile(root.filePath("crate_b/Cargo.toml"),
              "[package]\n"
              "name = \"crate_b\"\n"
              "version = \"0.1.0\"\n");
    writeFile(root.filePath("crate_b/src/main.rs"), "mod config;\nfn main() {}\n");
    writeFile(root.filePath("crate_b/src/config.rs"),
              "/// B's settings.\n"
              "pub struct Settings;\n"
              "\n"
              "/// B's loader.\n"
              "pub fn load() -> Settings { Settings }\n");
}

} // namespace

TEST_CASE("RustAnalyzer::analyze disambiguates nested modules of the same name "
          "across crates, and the types/functions they own", "[rustanalyzer]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QDir root(tmp.path());
    buildNestedModuleCollisionWorkspace(root);

    const RepoFileIndex index = buildRepoFileIndex(root.absolutePath());
    ir::Repo repo;
    QString error;
    REQUIRE(RustAnalyzer().analyze(index, &repo, &error));

    // Two distinct "config" modules in the IR, each correctly tagged with
    // its own owning crate via parentLabel — not one module shared (or
    // misattributed) between them.
    int configModuleCount = 0;
    QString parentOfA, parentOfB;
    for (const ir::Module& m : repo.modules()) {
        if (m.label != QStringLiteral("config")) continue;
        ++configModuleCount;
    }
    CHECK(configModuleCount == 2);
    int parentALabelCount = 0, parentBLabelCount = 0;
    for (const ir::Module& m : repo.modules()) {
        if (m.label != QStringLiteral("config")) continue;
        if (m.parentLabel == QStringLiteral("crate_a")) ++parentALabelCount;
        if (m.parentLabel == QStringLiteral("crate_b")) ++parentBLabelCount;
    }
    CHECK(parentALabelCount == 1);
    CHECK(parentBLabelCount == 1);

    // Each crate's own "config" module owns exactly one "load" free function
    // with its own doc — not merged, not cross-attributed.
    bool sawLoadADoc = false, sawLoadBDoc = false;
    int loadCount = 0;
    for (const ir::Module& m : repo.modules()) {
        if (m.label != QStringLiteral("config")) continue;
        for (const ir::Function& f : m.freeFunctions) {
            if (f.label != QStringLiteral("load")) continue;
            ++loadCount;
            if (m.parentLabel == QStringLiteral("crate_a"))
                sawLoadADoc = f.description == QStringLiteral("A's loader.");
            if (m.parentLabel == QStringLiteral("crate_b"))
                sawLoadBDoc = f.description == QStringLiteral("B's loader.");
        }
    }
    CHECK(loadCount == 2);
    CHECK(sawLoadADoc);
    CHECK(sawLoadBDoc);

    // Two distinct "Settings" types, each tagged with the owning crate via
    // ownerCrate (since moduleLabel alone, "config", is ambiguous), each
    // keeping its own doc.
    int settingsCount = 0;
    bool sawSettingsADoc = false, sawSettingsBDoc = false;
    for (const ir::Type& t : repo.types()) {
        if (t.label != QStringLiteral("Settings")) continue;
        ++settingsCount;
        CHECK(t.moduleLabel == QStringLiteral("config"));
        if (t.ownerCrate == QStringLiteral("crate_a"))
            sawSettingsADoc = t.description == QStringLiteral("A's settings.");
        if (t.ownerCrate == QStringLiteral("crate_b"))
            sawSettingsBDoc = t.description == QStringLiteral("B's settings.");
    }
    CHECK(settingsCount == 2);
    CHECK(sawSettingsADoc);
    CHECK(sawSettingsBDoc);
}
