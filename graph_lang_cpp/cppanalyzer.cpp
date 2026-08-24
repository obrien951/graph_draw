#include "cppanalyzer.h"
#include "repofileindex.h"
#include "sourcetext.h"
#include "ir.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

// Everything in this anonymous namespace arrived VERBATIM from
// graph_analyze/repoanalyzer.cpp. Do not "improve" it here: tests/test_repoanalyzer.cpp
// is the regression contract for the move and must pass unedited.
namespace {

struct ModuleInfo {
    QString       name;
    QString       type;          // "library", "executable", or "directory"
    QString       relDir;        // directory (relative to repo root) it lives in
    QSet<QString> sourceFiles;   // absolute paths of owned sources
    QStringList   linkLibs;      // names from target_link_libraries (module deps)
    QString       description;
};

struct ClassInfo {
    QString     name;
    QString     file;            // absolute path where the definition was found
    QString     relFile;         // path relative to repo root (for descriptions)
    QString     keyword;         // "class" or "struct"
    QStringList bases;           // base-class identifiers
    QStringList methods;         // member function names
    QString     description;
    QString     moduleName;      // owning module, resolved after scanning
};

const QStringList& cppSuffixes()
{
    static const QStringList exts = {".cpp", ".cc", ".cxx", ".c++",
                                     ".h", ".hpp", ".hh", ".hxx"};
    return exts;
}

bool isCppSource(const QString& fileName)
{
    for (const QString& e : cppSuffixes())
        if (fileName.endsWith(e, Qt::CaseInsensitive))
            return true;
    return false;
}

// Generated files that would pollute the graph with noise.
bool isGeneratedSource(const QString& fileName)
{
    return fileName.startsWith(QLatin1String("moc_")) ||
           fileName.startsWith(QLatin1String("qrc_")) ||
           fileName.startsWith(QLatin1String("ui_")) ||
           fileName.endsWith(QLatin1String("_autogen.cpp")) ||
           fileName == QLatin1String("mocs_compilation.cpp");
}

// Words that can appear directly before a '(' but never name a member function:
// control-flow keywords plus primitive types / declaration qualifiers that show
// up in casts, function-pointer types, and default arguments.
bool isNonFunctionWord(const QString& w)
{
    static const QSet<QString> kw = {
        // control flow
        "if", "for", "while", "switch", "return", "sizeof", "catch",
        "do", "else", "case", "new", "delete", "throw", "and", "or",
        "not", "alignof", "decltype", "static_assert",
        // qualifiers / specifiers
        "explicit", "const", "constexpr", "static", "virtual", "inline",
        "friend", "using", "typedef", "namespace", "template", "operator",
        "auto", "mutable", "volatile", "register", "extern", "noexcept",
        // primitive types (as seen in casts / function-pointer decls)
        "void", "int", "bool", "char", "short", "long", "float", "double",
        "unsigned", "signed", "wchar_t", "size_t", "qreal"
    };
    return kw.contains(w);
}

// From a class body (comments/strings already blanked), extract member function
// names. Inline bodies and nested braces are collapsed so call sites and nested
// declarations don't leak in. Constructors, destructors, and operators are
// skipped to keep the graph focused on named behaviour.
QStringList extractMethods(const QString& body, const QString& className)
{
    QString decls;
    int depth = 0;
    for (int i = 0; i < body.size(); ++i) {
        const QChar c = body[i];
        if (c == '{') {
            if (depth == 0) decls += ';';
            ++depth;
        } else if (c == '}') {
            if (depth > 0) --depth;
        } else if (depth == 0) {
            decls += c;
        }
    }

    QStringList methods;
    QSet<QString> seen;
    static const QRegularExpression re(QStringLiteral("(~?[A-Za-z_]\\w*)\\s*\\("));
    for (const QString& seg : decls.split(QLatin1Char(';'))) {
        const QRegularExpressionMatch m = re.match(seg);
        if (!m.hasMatch()) continue;
        const QString name = m.captured(1);
        if (seg.left(m.capturedStart(1)).trimmed().isEmpty())  continue;  // ctor/dtor
        if (name == className || name == '~' + className)       continue;
        if (name.startsWith(QLatin1String("operator")))         continue;
        if (isNonFunctionWord(name))                            continue;
        if (!seen.contains(name)) { seen.insert(name); methods.append(name); }
    }
    return methods;
}

// Strip access specifiers / qualifiers from a base-class token, returning the
// bare identifier (e.g. "public virtual Base<T>" -> "Base").
QString baseIdentifier(QString token)
{
    token = token.simplified();
    static const QRegularExpression drop(
        QStringLiteral("\\b(public|protected|private|virtual)\\b"));
    token.remove(drop);
    const int lt = token.indexOf(QLatin1Char('<'));
    if (lt >= 0) token = token.left(lt);
    token = token.simplified();
    const int sep = token.lastIndexOf(QLatin1String("::"));
    if (sep >= 0) token = token.mid(sep + 2);
    return token.trimmed();
}

// Extract the whitespace-separated arguments of the first `command(` ... `)` call
// found from position `openParen`, advancing `*end` past the closing paren.
QStringList readCommandArgs(const QString& text, int openParen, int* end)
{
    int depth = 0;
    int i = openParen;
    QString args;
    for (; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == '(') { ++depth; if (depth == 1) continue; }
        if (c == ')') { --depth; if (depth == 0) { ++i; break; } }
        args += c;
    }
    if (end) *end = i;
    return args.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

} // namespace

QString CppAnalyzer::id() const
{
    return QStringLiteral("cpp");
}

bool CppAnalyzer::detect(const RepoFileIndex& index) const
{
    if (!index.named(QStringLiteral("CMakeLists.txt")).isEmpty())
        return true;
    for (const QString& path : index.withSuffix(cppSuffixes()))
        if (!isGeneratedSource(QFileInfo(path).fileName()))
            return true;
    return false;
}

bool CppAnalyzer::analyze(const RepoFileIndex& index, ir::Repo* repo, QString* error) const
{
    if (!repo) return false;

    const QString rootPath = index.root();
    const QDir root(rootPath);

    // Phase 1 (the filesystem walk) now lives in RepoFileIndex; select from it.
    QStringList cppFiles;
    for (const QString& path : index.withSuffix(cppSuffixes()))
        if (!isGeneratedSource(QFileInfo(path).fileName()))
            cppFiles.append(path);
    const QStringList cmakeFiles = index.named(QStringLiteral("CMakeLists.txt"));

    if (cppFiles.isEmpty()) {
        if (error) *error = QStringLiteral("No C++ source files found under ") + rootPath;
        return false;
    }

    // ── 2. Parse CMake targets into modules ──────────────────────────────────
    QList<ModuleInfo> modules;
    QHash<QString, int> moduleByName;   // target name -> index in modules
    QHash<QString, QString> fileToModule;

    static const QSet<QString> cmakeKeywords = {
        "STATIC", "SHARED", "MODULE", "INTERFACE", "OBJECT", "ALIAS", "IMPORTED",
        "EXCLUDE_FROM_ALL", "WIN32", "MACOSX_BUNDLE", "GLOBAL"
    };

    for (const QString& cmakePath : cmakeFiles) {
        QFile f(cmakePath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString text = sourcetext::blankC(QString::fromUtf8(f.readAll()));
        const QString cmakeDir = QFileInfo(cmakePath).absolutePath();

        static const QRegularExpression cmd(
            QStringLiteral("\\b(add_library|add_executable)\\s*\\("));
        auto it = cmd.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            int end = 0;
            const QStringList args = readCommandArgs(text, m.capturedEnd() - 1, &end);
            if (args.isEmpty()) continue;

            ModuleInfo mod;
            mod.name = args.first();
            mod.type = (m.captured(1) == QLatin1String("add_library"))
                           ? QStringLiteral("library") : QStringLiteral("executable");
            mod.relDir = root.relativeFilePath(cmakeDir);
            if (mod.relDir.isEmpty()) mod.relDir = QStringLiteral(".");

            for (int a = 1; a < args.size(); ++a) {
                const QString arg = args[a];
                if (cmakeKeywords.contains(arg))       continue;
                if (arg.contains(QLatin1Char('$')))    continue;  // unresolved var
                if (!isCppSource(arg))                 continue;
                const QString abs = QFileInfo(QDir(cmakeDir), arg).absoluteFilePath();
                mod.sourceFiles.insert(abs);
                fileToModule.insert(abs, mod.name);
            }

            if (!moduleByName.contains(mod.name)) {
                moduleByName.insert(mod.name, modules.size());
                modules.append(mod);
            } else {
                ModuleInfo& existing = modules[moduleByName[mod.name]];
                existing.sourceFiles.unite(mod.sourceFiles);
            }
        }

        // target_link_libraries(<target> ... <libs>)  -> inter-module deps
        static const QRegularExpression link(
            QStringLiteral("\\btarget_link_libraries\\s*\\("));
        auto lit = link.globalMatch(text);
        while (lit.hasNext()) {
            const QRegularExpressionMatch m = lit.next();
            int end = 0;
            const QStringList args = readCommandArgs(text, m.capturedEnd() - 1, &end);
            if (args.isEmpty() || !moduleByName.contains(args.first())) continue;
            ModuleInfo& mod = modules[moduleByName[args.first()]];
            for (int a = 1; a < args.size(); ++a) {
                const QString lib = args[a];
                if (lib == QLatin1String("PUBLIC") || lib == QLatin1String("PRIVATE")
                    || lib == QLatin1String("INTERFACE")) continue;
                mod.linkLibs.append(lib);
            }
        }
    }

    // ── 3. Directory fallback for files not owned by any CMake target ─────────
    for (const QString& file : cppFiles) {
        if (fileToModule.contains(file)) continue;
        const QString rel = root.relativeFilePath(file);
        const int slash = rel.indexOf(QLatin1Char('/'));
        const QString dirName = slash >= 0 ? rel.left(slash) : QStringLiteral("(root)");
        const QString modName = dirName + QStringLiteral(" (dir)");
        if (!moduleByName.contains(modName)) {
            ModuleInfo mod;
            mod.name   = modName;
            mod.type   = QStringLiteral("directory");
            mod.relDir = dirName;
            moduleByName.insert(modName, modules.size());
            modules.append(mod);
        }
        modules[moduleByName[modName]].sourceFiles.insert(file);
        fileToModule.insert(file, modName);
    }

    // ── 4. Scan sources for classes and their members ────────────────────────
    QList<ClassInfo> classes;
    QHash<QString, int> classByName;                 // class name -> index
    QHash<QString, QStringList> classesInFile;       // abs file -> class names
    QHash<QString, QStringList> includesInFile;      // abs file -> included basenames

    static const QRegularExpression classRe(
        QStringLiteral("\\b(class|struct)\\s+([A-Za-z_]\\w*)\\b"
                       "(?:\\s+final)?\\s*(?::([^{;]*))?\\{"));
    static const QRegularExpression includeRe(
        QStringLiteral("#\\s*include\\s*\"([^\"]+)\""));

    for (const QString& file : cppFiles) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString raw  = QString::fromUtf8(f.readAll());
        const QString code = sourcetext::blankC(raw);
        const QStringList rawLines = raw.split(QLatin1Char('\n'));
        const QString relFile = root.relativeFilePath(file);

        // Local includes (for class -> class "uses" edges).
        {
            auto it = includeRe.globalMatch(raw);
            while (it.hasNext())
                includesInFile[file].append(QFileInfo(it.next().captured(1)).fileName());
        }

        auto it = classRe.globalMatch(code);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();

            // Reject "enum class" and "enum struct".
            const QString pre = code.left(m.capturedStart()).trimmed();
            if (pre.endsWith(QLatin1String("enum"))) continue;

            ClassInfo ci;
            ci.keyword = m.captured(1);
            ci.name    = m.captured(2);
            ci.file    = file;
            ci.relFile = relFile;

            for (const QString& b : m.captured(3).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString id = baseIdentifier(b);
                if (!id.isEmpty()) ci.bases.append(id);
            }

            const int open = m.capturedEnd() - 1;
            const int j = sourcetext::matchingBrace(code, open);
            ci.methods = extractMethods(code.mid(open + 1, j - open - 2), ci.name);

            const int declLine = sourcetext::lineOf(code, m.capturedStart());
            const QString lead = sourcetext::leadingComment(rawLines, declLine);
            ci.description = lead.isEmpty()
                ? QStringLiteral("%1 %2 declared in %3.")
                      .arg(ci.keyword, ci.name, relFile)
                : lead;

            if (classByName.contains(ci.name)) continue;   // first definition wins
            classByName.insert(ci.name, classes.size());
            classesInFile[file].append(ci.name);
            classes.append(ci);
        }
    }

    // Resolve each class's owning module from its file.
    for (ClassInfo& ci : classes)
        ci.moduleName = fileToModule.value(ci.file);

    // Finish module descriptions now that sources are known.
    for (ModuleInfo& mod : modules) {
        mod.description = QStringLiteral("CMake %1 target \"%2\" (%3), %4 source file(s).")
                              .arg(mod.type, mod.name, mod.relDir)
                              .arg(mod.sourceFiles.size());
        if (mod.type == QLatin1String("directory"))
            mod.description = QStringLiteral("Directory module \"%1\", %2 source file(s).")
                                  .arg(mod.relDir).arg(mod.sourceFiles.size());
    }

    // ── 5. Emit into the IR ──────────────────────────────────────────────────
    for (const ModuleInfo& mod : modules) {
        ir::Module m;
        m.label           = mod.name;
        m.description     = mod.description;
        m.dependsOnLabels = mod.linkLibs;
        repo->addModule(m);
    }

    // The "uses" set. THE ONE BEHAVIOURAL CHANGE IN THIS TASK: the original
    // iterated classesInFile.keys() — QHash order, i.e. arbitrary between builds
    // — which made the emitted edge array non-deterministic. Sorting the file
    // list fixes it. The resulting edge SET is identical.
    QStringList filesWithClasses = classesInFile.keys();
    filesWithClasses.sort();

    for (const ClassInfo& c : classes) {
        ir::Type t;
        t.label       = c.name;
        t.description = c.description;
        t.moduleLabel = c.moduleName;
        t.baseLabels  = c.bases;

        for (const QString& method : c.methods) {
            const QString qn = c.name + QStringLiteral("::") + method;
            ir::Function fn;
            fn.label       = qn;
            fn.description = QStringLiteral("Member function %1 of %2 %3 (%4).")
                                 .arg(qn, c.keyword, c.name, c.relFile);
            t.methods.append(fn);
        }

        // "uses": classes defined in headers this class's file includes.
        QSet<QString> usesSeen;
        for (const QString& inc : includesInFile.value(c.file)) {
            for (const QString& file : filesWithClasses) {
                if (QFileInfo(file).fileName() != inc) continue;
                for (const QString& other : classesInFile.value(file)) {
                    if (other == c.name || c.bases.contains(other)) continue;
                    if (usesSeen.contains(other)) continue;
                    usesSeen.insert(other);
                    t.usesLabels.append(other);
                }
            }
        }

        repo->addType(t);
    }

    return true;
}
