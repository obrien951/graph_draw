#include "graphscene.h"
#include "graphserializer.h"
#include "repoanalyzer.h"
#include "graphmerger.h"
#include "graphnode.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

// Command-line tool: scan a repository and write a module/class/function
// dependency graph in the graph_io JSON format.
//
//   repo_to_graph [--lang cpp|rust] [--merge existing.json] [--dry-run]
//                 <repo-root> [output.json]
//
// If no output path is given, the graph is written to
// <repo-root>/graphs/<repo-name>.generated.json. The ".generated" suffix keeps
// the tool from ever overwriting a hand-curated graph at graphs/<name>.json —
// this is unconditional and does not change based on --merge: the output path
// is always derived from the repo's directory name alone, never from
// whatever path --merge happens to name, so passing --merge at a curated
// graph can never redirect where the result is written.
int main(int argc, char* argv[])
{
    // GraphNode/GraphScene are QGraphicsObjects, so a QApplication must exist
    // even though this tool never opens a window. On headless machines run with
    // QT_QPA_PLATFORM=offscreen.
    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Scan a repository and write a module/class/function "
                        "dependency graph in the graph_io JSON format."));
    parser.addHelpOption();

    QCommandLineOption langOption(
        QStringLiteral("lang"),
        QStringLiteral("Restrict analysis to one language id (cpp, rust). Default: autodetect."),
        QStringLiteral("id"));
    QCommandLineOption mergeOption(
        QStringLiteral("merge"),
        QStringLiteral("Merge the freshly analyzed graph into this existing curated graph "
                        "instead of overwriting the output path from scratch."),
        QStringLiteral("existing.json"));
    QCommandLineOption dryRunOption(
        QStringLiteral("dry-run"),
        QStringLiteral("With --merge, report kept/added/stale counts without writing any file."));
    parser.addOption(langOption);
    parser.addOption(mergeOption);
    parser.addOption(dryRunOption);
    parser.addPositionalArgument(QStringLiteral("repo-root"), QStringLiteral("Repository root to scan."));
    parser.addPositionalArgument(QStringLiteral("output.json"),
        QStringLiteral("Output path. Default: <repo-root>/graphs/<name>.generated.json."),
        QStringLiteral("[output.json]"));

    parser.process(app);

    QTextStream out(stdout);
    QTextStream err(stderr);

    const QStringList positional = parser.positionalArguments();
    if (positional.isEmpty()) {
        err << "Usage: " << QFileInfo(app.arguments().value(0)).fileName()
            << " [--lang cpp|rust] [--merge existing.json] [--dry-run] <repo-root> [output.json]\n";
        return 2;
    }

    if (parser.isSet(dryRunOption) && !parser.isSet(mergeOption)) {
        err << "--dry-run has no effect without --merge.\n";
        return 2;
    }

    const QString repoRoot = positional.at(0);
    const QDir root(repoRoot);

    // Output path is always derived from the repo root alone — see the file
    // comment above for why --merge must never influence this.
    QString outPath = positional.value(1);
    if (outPath.isEmpty()) {
        const QString name = root.dirName().isEmpty()
                                 ? QStringLiteral("repo") : root.dirName();
        root.mkpath(QStringLiteral("graphs"));
        outPath = root.filePath(QStringLiteral("graphs/") + name
                                + QStringLiteral(".generated.json"));
    }

    GraphScene generated;
    RepoAnalyzer analyzer;
    if (parser.isSet(langOption))
        analyzer.setLanguage(parser.value(langOption));
    if (!analyzer.analyze(repoRoot, &generated)) {
        err << "Analysis failed: " << analyzer.lastError() << '\n';
        return 1;
    }

    GraphSerializer serializer;

    if (parser.isSet(mergeOption)) {
        const QString mergePath = parser.value(mergeOption);
        GraphScene curated;
        if (!serializer.loadFromFile(mergePath, &curated)) {
            err << "Could not load --merge graph " << mergePath << ": "
                << serializer.lastError() << '\n';
            return 1;
        }

        const GraphMerger::Stats stats = GraphMerger::merge(&curated, &generated);
        out << QStringLiteral("kept %1 · added %2 · stale %3\n")
                   .arg(stats.kept).arg(stats.added).arg(stats.stale);

        if (parser.isSet(dryRunOption))
            return 0;

        if (!serializer.saveToFile(outPath, &curated)) {
            err << "Could not write graph: " << serializer.lastError() << '\n';
            return 1;
        }
        out << "Wrote " << outPath << "\n";
        return 0;
    }

    if (!serializer.saveToFile(outPath, &generated)) {
        err << "Could not write graph: " << serializer.lastError() << '\n';
        return 1;
    }

    // Report a short summary.
    int modules = 0, classes = 0, functions = 0;
    for (GraphNode* n : generated.nodes()) {
        switch (n->kind()) {
        case NodeType::Module:   ++modules;   break;
        case NodeType::Class:    ++classes;   break;
        case NodeType::Function: ++functions; break;
        }
    }
    out << "Wrote " << outPath << "\n"
        << "  modules:   " << modules   << "\n"
        << "  classes:   " << classes   << "\n"
        << "  functions: " << functions << "\n"
        << "  languages: " << analyzer.languagesUsed().join(QStringLiteral(", ")) << "\n";
    return 0;
}
