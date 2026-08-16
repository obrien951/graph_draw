#include "graphscene.h"
#include "graphserializer.h"
#include "repoanalyzer.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

// Command-line tool: scan a C++ repository and write a module/class/function
// dependency graph in the graph_io JSON format.
//
//   repo_to_graph <repo-root> [output.json]
//
// If no output path is given, the graph is written to
// <repo-root>/graphs/<repo-name>.generated.json. The ".generated" suffix keeps
// the tool from ever overwriting a hand-curated graph at graphs/<name>.json.
int main(int argc, char* argv[])
{
    // GraphNode/GraphScene are QGraphicsObjects, so a QApplication must exist
    // even though this tool never opens a window. On headless machines run with
    // QT_QPA_PLATFORM=offscreen.
    QApplication app(argc, argv);

    QTextStream out(stdout);
    QTextStream err(stderr);

    const QStringList args = app.arguments();
    if (args.size() < 2) {
        err << "Usage: " << QFileInfo(args.value(0)).fileName()
            << " <repo-root> [output.json]\n";
        return 2;
    }

    const QString repoRoot = args.at(1);
    const QDir root(repoRoot);
    QString outPath = args.value(2);
    if (outPath.isEmpty()) {
        const QString name = root.dirName().isEmpty()
                                 ? QStringLiteral("repo") : root.dirName();
        root.mkpath(QStringLiteral("graphs"));
        outPath = root.filePath(QStringLiteral("graphs/") + name
                                + QStringLiteral(".generated.json"));
    }

    GraphScene scene;
    RepoAnalyzer analyzer;
    if (!analyzer.analyze(repoRoot, &scene)) {
        err << "Analysis failed: " << analyzer.lastError() << '\n';
        return 1;
    }

    GraphSerializer serializer;
    if (!serializer.saveToFile(outPath, &scene)) {
        err << "Could not write graph: " << serializer.lastError() << '\n';
        return 1;
    }

    // Report a short summary.
    int modules = 0, classes = 0, functions = 0;
    for (GraphNode* n : scene.nodes()) {
        switch (n->kind()) {
        case NodeType::Module:   ++modules;   break;
        case NodeType::Class:    ++classes;   break;
        case NodeType::Function: ++functions; break;
        }
    }
    out << "Wrote " << outPath << "\n"
        << "  modules:   " << modules   << "\n"
        << "  classes:   " << classes   << "\n"
        << "  functions: " << functions << "\n";
    return 0;
}
