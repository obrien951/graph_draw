#include "rust_analyzer.h"
#include "repofileindex.h"
#include "toml_document.h"
#include "rust_file_items.h"
#include "rustlex.h"
#include "ir.h"

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QVector>

QString RustAnalyzer::id() const
{
    return QStringLiteral("rust");
}

bool RustAnalyzer::detect(const RepoFileIndex& index) const
{
    return !index.named("Cargo.toml").isEmpty();
}

namespace {

// Cargo accepts either spelling in different contexts (package.name vs. a
// dependency key); compare on this normalized form so "my-crate" and
// "my_crate" resolve to the same crate.
QString normalizeCrateName(const QString& name)
{
    QString n = name;
    return n.replace(QLatin1Char('-'), QLatin1Char('_'));
}

// Types and free functions are merged/deduped per (owning crate, owning
// module, bare label), never by bare label alone, and never by (module,
// label) alone either — a module's bare name (e.g. "config") is only unique
// *within* a crate, not across the repo, since two different crates can each
// have their own depth-1 src/config.rs. '\x1f' can't appear in any part,
// same separator convention GraphBuilder uses for its own composite edge
// keys.
QString itemKey(const QString& ownerCrate, const QString& ownerModule, const QString& label)
{
    return ownerCrate + QLatin1Char('\x1f') + ownerModule + QLatin1Char('\x1f') + label;
}

// A module's bare label is only unique within its own crate (two crates can
// each have a child module named "config"), so the index that lets the
// file-scan pass find "the module this file belongs to" must be keyed the
// same crate-qualified way, or two crates' same-named child modules would
// alias to whichever one was inserted last.
QString moduleKey(const QString& ownerCrate, const QString& moduleBareLabel)
{
    return ownerCrate + QLatin1Char('\x1f') + moduleBareLabel;
}

struct CrateInfo {
    QString      name;
    QString      dir;   // absolute directory containing this crate's Cargo.toml
    TomlDocument doc;
};

// Depth-1 nested module names under <crateDir>/src, derived from the shared
// file index (already target/-free) rather than a fresh filesystem walk. A
// top-level src/main.rs or src/lib.rs IS the crate root, not a child; any
// other direct src/ file, or any src/<name>/ directory, is exactly one child
// module no matter how deep its own contents go — a decoy file three levels
// under src/db/ must never surface as its own module.
QStringList nestedModuleNames(const RepoFileIndex& index, const QString& crateDir)
{
    const QString srcPrefix = crateDir + QStringLiteral("/src/");
    QSet<QString> names;
    for (const QString& path : index.files()) {
        if (!path.startsWith(srcPrefix))
            continue;
        const QString remainder = path.mid(srcPrefix.length());
        const int slash = remainder.indexOf(QLatin1Char('/'));
        if (slash >= 0) {
            names.insert(remainder.left(slash));
            continue;
        }
        if (!remainder.endsWith(QLatin1String(".rs")))
            continue;
        const QString stem = remainder.left(remainder.length() - 3);
        if (stem == QLatin1String("main") || stem == QLatin1String("lib"))
            continue;
        names.insert(stem);
    }
    QStringList sorted = names.values();
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

// Dependency names a crate's own Cargo.toml declares (regular, dev, build),
// unioned with the workspace-level dependency table when one exists. Whether
// a candidate actually names a crate this analyzer discovered is decided by
// the caller; an external crate (serde, tokio, ...) simply isn't in that set
// and the candidate is dropped there, same as any other unresolved label.
QStringList dependencyCandidates(const TomlDocument& doc, const QStringList& workspaceDeps)
{
    QSet<QString> names;
    static const QStringList kPrefixes = {
        QStringLiteral("dependencies"),
        QStringLiteral("dev-dependencies"),
        QStringLiteral("build-dependencies"),
    };
    for (const QString& prefix : kPrefixes)
        for (const QString& n : doc.keys(prefix))
            names.insert(n);
    for (const QString& n : workspaceDeps)
        names.insert(n);
    QStringList result = names.values();
    std::sort(result.begin(), result.end());
    return result;
}

// Which module owns a given .rs file under <crateDir>/src — the crate root
// for main.rs/lib.rs, or the depth-1 child module a direct sibling file or
// directory resolves to. Mirrors nestedModuleNames' own rule exactly, so a
// file's type/function nodes always land on the same module its name did.
QString owningModuleLabel(const QString& crateLabel, const QString& crateDir, const QString& filePath)
{
    const QString srcPrefix = crateDir + QStringLiteral("/src/");
    if (!filePath.startsWith(srcPrefix))
        return crateLabel;
    const QString remainder = filePath.mid(srcPrefix.length());
    const int slash = remainder.indexOf(QLatin1Char('/'));
    if (slash >= 0)
        return remainder.left(slash);
    const QString stem = remainder.endsWith(QLatin1String(".rs"))
        ? remainder.left(remainder.length() - 3) : remainder;
    if (stem == QLatin1String("main") || stem == QLatin1String("lib"))
        return crateLabel;
    return stem;
}

// One scanned .rs file: which module its free functions/types belong to,
// what scanRustFile found, and its raw lines (leadingDoc needs them, and
// blanking them again per description lookup would be wasted work).
struct FileScan {
    QString        relPath;      // repo-root-relative, for fallback descriptions
    QString        ownerModule;
    QString        ownerCrate;   // the crate this file belongs to, regardless of
                                 // which of its modules owns the file directly —
                                 // `use` paths resolve against the whole crate,
                                 // not just the one module that declared them.
    RustFileItems  items;
    QStringList    rawLines;
};

} // namespace

bool RustAnalyzer::analyze(const RepoFileIndex& index, ir::Repo* repo, QString* error) const
{
    // Crates from every Cargo.toml, deduped by directory and sorted by crate
    // name. Nested modules are depth-1 and derived from src/ paths, so no mod
    // resolution is needed and the tree cannot disagree with itself.
    const auto cargoFiles = index.named("Cargo.toml");

    QSet<QString>     seenDirectories;
    QVector<CrateInfo> crates;
    QStringList        workspaceDeps;

    for (const QString& cargoPath : cargoFiles) {
        // Directory containing the manifest — NOT QDir(cargoPath).absolutePath(),
        // which (being handed a file path) returns the file path itself rather
        // than its parent.
        const QString dirPath = QFileInfo(cargoPath).absolutePath();

        if (seenDirectories.contains(dirPath))
            continue;
        seenDirectories.insert(dirPath);

        QFile file(cargoPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (error) *error = QString("Failed to open Cargo.toml at %1").arg(cargoPath);
            return false;
        }
        const QString cargoContent = QString::fromUtf8(file.readAll());
        file.close();

        TomlDocument doc = TomlDocument::parse(cargoContent);

        // A workspace root may declare [workspace.dependencies] while having
        // no [package] of its own (a "virtual manifest") — still worth
        // reading even though it contributes no crate module.
        for (const QString& n : doc.keys(QStringLiteral("workspace.dependencies")))
            workspaceDeps.append(n);

        const QString crateName = doc.value(QStringLiteral("package.name"));
        if (crateName.isEmpty())
            continue;

        crates.append(CrateInfo{crateName, dirPath, std::move(doc)});
    }

    std::sort(crates.begin(), crates.end(),
              [](const CrateInfo& a, const CrateInfo& b) { return a.name < b.name; });

    QHash<QString, QString> crateByNormalizedName; // normalized -> real label
    for (const CrateInfo& c : crates)
        crateByNormalizedName.insert(normalizeCrateName(c.name), c.name);

    // Module index as each module lands in repo->modules(), kept so the
    // file-scan pass below can append free functions without a second lookup
    // mechanism. Keyed by (owning crate, bare module label) — a bare module
    // name like "config" is only unique within its own crate.
    QHash<QString, int> moduleIndexByLabel;

    for (const CrateInfo& c : crates) {
        ir::Module module;
        module.label             = c.name;
        module.description       = QString("Rust crate: %1").arg(c.name);
        module.childModuleLabels = nestedModuleNames(index, c.dir);

        QSet<QString> deps;
        for (const QString& candidate : dependencyCandidates(c.doc, workspaceDeps)) {
            const auto it = crateByNormalizedName.constFind(normalizeCrateName(candidate));
            if (it == crateByNormalizedName.constEnd())
                continue;          // not one of ours — external dependency
            if (it.value() == c.name)
                continue;          // no self-edges
            deps.insert(it.value());
        }
        QStringList depList = deps.values();
        std::sort(depList.begin(), depList.end());
        module.dependsOnLabels = depList;

        repo->addModule(module);
        moduleIndexByLabel.insert(moduleKey(c.name, module.label), repo->modules().size() - 1);

        for (const QString& childName : module.childModuleLabels) {
            ir::Module child;
            child.label       = childName;
            child.description = QString("Rust module: %1").arg(childName);
            child.parentLabel = c.name;
            repo->addModule(child);
            moduleIndexByLabel.insert(moduleKey(c.name, childName), repo->modules().size() - 1);
        }
    }

    // ── Per-file scan: types, methods, free functions, use-paths ────────────
    // scanRustFile does the actual parsing; this pass only decides which
    // module/type in the IR each of its findings belongs to.
    const QDir root(index.root());
    QVector<FileScan> fileScans;

    for (const CrateInfo& c : crates) {
        const QString srcPrefix = c.dir + QStringLiteral("/src/");
        QStringList rsFiles;
        for (const QString& path : index.withSuffix({QStringLiteral(".rs")}))
            if (path.startsWith(srcPrefix))
                rsFiles.append(path);
        std::sort(rsFiles.begin(), rsFiles.end());

        for (const QString& path : rsFiles) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue; // unreadable file contributes nothing, same as a missing one
            const QString content = QString::fromUtf8(f.readAll());
            f.close();

            FileScan scan;
            scan.relPath     = root.relativeFilePath(path);
            scan.ownerModule = owningModuleLabel(c.name, c.dir, path);
            scan.ownerCrate  = c.name;
            scan.items       = scanRustFile(content);
            scan.rawLines    = content.split(QLatin1Char('\n'));
            fileScans.append(std::move(scan));
        }
    }

    // Types/methods merge by (owning crate, owning module, label), not label
    // alone and not (module, label) alone either — a module's bare name is
    // only unique within its own crate, so two crates each with their own
    // "config" child module must never merge each other's same-named types.
    // An impl in one file for a struct declared in another *file of the same
    // module of the same crate* is still the normal shape for a trait + its
    // impls living alongside each other, and still merges correctly.
    QHash<QString, int>             typeIndexByKey;
    QHash<int, QSet<QString>>       typesTouchedByFile;  // fileScans index -> composite keys
    QSet<QString>                   functionKeysSeen;    // composite (crate, module, label) dedup

    // bare label -> every (ownerCrate, ownerModule) that declared it, for
    // `use` resolution below: a `use` path only ever names something in the
    // writer's own crate, never a sibling crate's unrelated same-named item
    // — including when that sibling crate's module happens to share the same
    // bare name (two crates each with their own "config"), so matching must
    // be by crate identity, not by module bare-name membership.
    struct ModuleOwner { QString crate; QString module; };
    QHash<QString, QVector<ModuleOwner>> moduleOwnersByBareLabel;

    for (int fi = 0; fi < fileScans.size(); ++fi) {
        const FileScan& scan = fileScans.at(fi);
        QSet<QString>& touched = typesTouchedByFile[fi];

        for (const RustFileItems::Type& t : scan.items.types) {
            const QString key = itemKey(scan.ownerCrate, scan.ownerModule, t.label);
            int idx;
            const auto it = typeIndexByKey.constFind(key);
            if (it == typeIndexByKey.constEnd()) {
                ir::Type nt;
                nt.label       = t.label;
                nt.moduleLabel = scan.ownerModule;
                // Only set when the owner is a NESTED module (not the crate
                // root) — a bare crate name is already globally unique, so
                // tagging it here would be noise, and GraphBuilder's
                // disambiguation path specifically looks for this to mean
                // "moduleLabel might collide with another crate's module of
                // the same name."
                if (scan.ownerModule != scan.ownerCrate)
                    nt.ownerCrate = scan.ownerCrate;
                repo->addType(nt);
                idx = repo->types().size() - 1;
                typeIndexByKey.insert(key, idx);
                moduleOwnersByBareLabel[t.label].append({scan.ownerCrate, scan.ownerModule});
            } else {
                idx = it.value();
            }
            touched.insert(key);

            ir::Type& rt = repo->types()[idx];
            if (rt.description.isEmpty()) {
                const QString doc = rustlex::leadingDoc(scan.rawLines, t.line);
                rt.description = doc.isEmpty()
                    ? QString("Rust item %1 declared in %2.").arg(t.label, scan.relPath)
                    : doc;
            }
            for (const QString& trait : t.implementsTraits)
                if (!rt.baseLabels.contains(trait))
                    rt.baseLabels.append(trait);
            for (const RustFileItems::Item& m : t.methods) {
                const QString qualified = t.label + QStringLiteral("::") + m.label;
                bool exists = false;
                for (const ir::Function& existing : rt.methods)
                    if (existing.label == qualified) { exists = true; break; }
                if (exists) continue;
                ir::Function fn;
                fn.label = qualified;
                const QString doc = rustlex::leadingDoc(scan.rawLines, m.line);
                fn.description = doc.isEmpty()
                    ? QString("Method %1 of %2.").arg(m.label, t.label)
                    : doc;
                rt.methods.append(fn);
            }
        }

        const QString ownerKey = moduleKey(scan.ownerCrate, scan.ownerModule);
        if (!moduleIndexByLabel.contains(ownerKey))
            continue; // defensive: every owner this pass produces was just added above
        ir::Module& ownerModule = repo->modules()[moduleIndexByLabel.value(ownerKey)];
        for (const RustFileItems::Item& fn : scan.items.functions) {
            const QString fnKey = itemKey(scan.ownerCrate, scan.ownerModule, fn.label);
            if (functionKeysSeen.contains(fnKey)) continue; // first definition wins, per-crate-per-module
            functionKeysSeen.insert(fnKey);
            ir::Function f;
            f.label = fn.label;
            const QString doc = rustlex::leadingDoc(scan.rawLines, fn.line);
            f.description = doc.isEmpty()
                ? QString("Function %1 declared in %2.").arg(fn.label, scan.relPath)
                : doc;
            ownerModule.freeFunctions.append(f);
        }
    }

    // "uses": file-granular, mirroring the C++ include heuristic's fidelity —
    // every type a file declares or implements is considered to use whatever
    // repo-defined type any of that file's `use` paths named. A `use` path
    // only ever names something in the writer's own crate (an unqualified
    // leaf can't reach into a sibling crate), so resolution is scoped by
    // crate IDENTITY, not by module bare-name membership — two crates can
    // each have a module bare-named "config", so "is this owner's module
    // name one of my crate's module names" is not a safe test; "is this
    // owner's crate my crate" is. A bare label that resolves to exactly one
    // module within this crate is accepted; one that's only defined in some
    // other crate is external and dropped (same as before); one that's
    // ambiguous even within this single crate (two of its own modules both
    // declaring the same bare name, and the `use` path too short to say
    // which) is dropped rather than guessed at — the same "don't resolve
    // what you can't" philosophy as every other label here.
    for (int fi = 0; fi < fileScans.size(); ++fi) {
        const FileScan& scan = fileScans.at(fi);

        QSet<QString> resolvedUses; // bare labels, unambiguous within this crate
        for (const QString& path : scan.items.usePaths) {
            const auto it = moduleOwnersByBareLabel.constFind(path);
            if (it == moduleOwnersByBareLabel.constEnd()) continue;
            int matches = 0;
            for (const ModuleOwner& owner : it.value())
                if (owner.crate == scan.ownerCrate) ++matches;
            if (matches == 1) resolvedUses.insert(path);
        }
        if (resolvedUses.isEmpty()) continue;

        for (const QString& touchedKey : typesTouchedByFile.value(fi)) {
            ir::Type& rt = repo->types()[typeIndexByKey.value(touchedKey)];
            for (const QString& used : resolvedUses) {
                if (used == rt.label) continue;
                if (rt.baseLabels.contains(used)) continue;
                if (!rt.usesLabels.contains(used))
                    rt.usesLabels.append(used);
            }
        }
    }

    return true;
}
