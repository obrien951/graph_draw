#include "repoanalyzer.h"
#include "graphscene.h"
#include "graphnode.h"
#include "graphedge.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextStream>

// ── Internal model ──────────────────────────────────────────────────────────────
namespace {

struct ModuleInfo {
    QString     name;
    QString     type;         // "library", "executable", or "directory"
    QString     relDir;       // directory (relative to repo root) it lives in
    QSet<QString> sourceFiles;// absolute paths of owned sources
    QStringList linkLibs;     // names from target_link_libraries (module deps)
    QString     description;
};

struct ClassInfo {
    QString     name;
    QString     file;         // absolute path where the definition was found
    QString     relFile;      // path relative to repo root (for descriptions)
    QString     keyword;      // "class" or "struct"
    QStringList bases;        // base-class identifiers
    QStringList methods;      // member function names
    QString     description;
    QString     moduleName;   // owning module, resolved after scanning
};

// Directories we never descend into.
bool isSkippedDir(const QString& name)
{
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return true;
    if (name.startsWith(QLatin1Char('.')))          // .git, .idea, .qt, ...
        return true;
    if (name == QLatin1String("build") ||
        name.startsWith(QLatin1String("cmake-build")) ||
        name == QLatin1String("node_modules") ||
        name.endsWith(QLatin1String("_autogen")) ||
        name == QLatin1String("CMakeFiles"))
        return true;
    return false;
}

bool isCppSource(const QString& fileName)
{
    static const QStringList exts = {".cpp", ".cc", ".cxx", ".c++",
                                     ".h", ".hpp", ".hh", ".hxx"};
    for (const QString& e : exts)
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

// Replace comments and string/char literals with spaces (newlines preserved) so
// declaration matching never trips over commented-out or quoted code, while line
// numbers stay aligned with the original text.
QString blankCommentsAndStrings(const QString& s)
{
    QString out;
    out.reserve(s.size());
    enum State { Code, Line, Block, Str, Chr } st = Code;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        const QChar n = (i + 1 < s.size()) ? s[i + 1] : QChar();
        auto keepNl = [&](QChar ch) { out += (ch == QLatin1Char('\n')) ? ch : QChar(' '); };
        switch (st) {
        case Code:
            if (c == '/' && n == '/') { st = Line;  out += ' '; }
            else if (c == '/' && n == '*') { st = Block; out += ' '; }
            else if (c == '"')  { st = Str; out += ' '; }
            else if (c == '\'') { st = Chr; out += ' '; }
            else out += c;
            break;
        case Line:
            if (c == '\n') { st = Code; out += '\n'; } else out += ' ';
            break;
        case Block:
            if (c == '*' && n == '/') { st = Code; out += "  "; ++i; } else keepNl(c);
            break;
        case Str:
            if (c == '\\') { out += ' '; if (i + 1 < s.size()) { keepNl(n); ++i; } }
            else if (c == '"') { st = Code; out += ' '; } else keepNl(c);
            break;
        case Chr:
            if (c == '\\') { out += ' '; if (i + 1 < s.size()) { keepNl(n); ++i; } }
            else if (c == '\'') { st = Code; out += ' '; } else keepNl(c);
            break;
        }
    }
    return out;
}

// Collect the contiguous comment block immediately above line declLine (0-based)
// in rawLines, stripped of comment markers. Empty if there is none.
QString leadingComment(const QStringList& rawLines, int declLine)
{
    QStringList collected;
    int i = declLine - 1;
    // Skip a single blank line between the comment and the declaration.
    while (i >= 0 && rawLines[i].trimmed().isEmpty() && collected.isEmpty())
        --i;
    for (; i >= 0; --i) {
        QString t = rawLines[i].trimmed();
        if (t.startsWith(QLatin1String("//"))) {
            t.remove(0, 2);
            collected.prepend(t.trimmed());
        } else if (t.endsWith(QLatin1String("*/")) || t.startsWith(QLatin1String("*"))
                   || t.startsWith(QLatin1String("/*"))) {
            t.remove(QRegularExpression(QStringLiteral("^/\\*+|\\*+/$|^\\*+")));
            collected.prepend(t.trimmed());
            if (rawLines[i].trimmed().startsWith(QLatin1String("/*")))
                break;
        } else {
            break;
        }
    }
    while (!collected.isEmpty() && collected.first().isEmpty())
        collected.removeFirst();
    return collected.join(QLatin1Char(' ')).simplified();
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
    // Build a "declarations only" stream: keep depth-0 text, replace the content
    // of any nested { } with a single ';' terminator.
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
    // A name immediately followed by '(' at declaration level. Only the first
    // match in a segment is the declared function; anything later is arguments
    // (e.g. `= QString()` default values), so we never scan past it.
    static const QRegularExpression re(QStringLiteral("(~?[A-Za-z_]\\w*)\\s*\\("));
    for (const QString& seg : decls.split(QLatin1Char(';'))) {
        const QRegularExpressionMatch m = re.match(seg);
        if (!m.hasMatch()) continue;
        const QString name = m.captured(1);
        // A member function declaration has a return type before its name; a
        // constructor/destructor does not, so a name at the very start is one of
        // those and is skipped along with operators.
        if (seg.left(m.capturedStart(1)).trimmed().isEmpty())  continue;  // ctor/dtor
        if (name == className || name == '~' + className)       continue;
        if (name.startsWith(QLatin1String("operator")))         continue;
        if (isNonFunctionWord(name))                            continue;
        if (!seen.contains(name)) { seen.insert(name); methods.append(name); }
    }
    return methods;
}

int lineOf(const QString& text, int offset)
{
    return text.left(offset).count(QLatin1Char('\n'));  // 0-based
}

// Strip access specifiers / qualifiers from a base-class token, returning the
// bare identifier (e.g. "public virtual Base<T>" → "Base").
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

} // namespace

// ── CMake target parsing ────────────────────────────────────────────────────────
namespace {

// Extract the whitespace-separated arguments of the first `command(` ... `)` call
// found from position `from`, advancing `from` past the closing paren.
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

// ── Analyze ─────────────────────────────────────────────────────────────────────

bool RepoAnalyzer::analyze(const QString& repoRoot, GraphScene* scene)
{
    m_lastError.clear();

    const QDir root(repoRoot);
    if (!root.exists()) {
        m_lastError = QStringLiteral("Repository root does not exist: ") + repoRoot;
        return false;
    }
    const QString rootPath = root.absolutePath();

    // ── 1. Walk the tree, collecting C++ sources and CMakeLists files ─────────
    QStringList cppFiles;
    QStringList cmakeFiles;
    {
        QDirIterator it(rootPath, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        // QDirIterator can't skip whole subtrees directly, so filter by path.
        while (it.hasNext()) {
            const QString path = it.next();
            const QFileInfo fi(it.fileInfo());
            const QString rel = QDir(rootPath).relativeFilePath(path);
            bool skip = false;
            for (const QString& part : rel.split(QLatin1Char('/')))
                if (isSkippedDir(part)) { skip = true; break; }
            if (skip || !fi.isFile()) continue;

            const QString name = fi.fileName();
            if (name == QLatin1String("CMakeLists.txt"))
                cmakeFiles.append(path);
            else if (isCppSource(name) && !isGeneratedSource(name))
                cppFiles.append(path);
        }
    }

    if (cppFiles.isEmpty()) {
        m_lastError = QStringLiteral("No C++ source files found under ") + rootPath;
        return false;
    }

    // ── 2. Parse CMake targets into modules ──────────────────────────────────
    QList<ModuleInfo> modules;
    QHash<QString, int> moduleByName;   // target name → index in modules
    QHash<QString, QString> fileToModule;

    static const QSet<QString> cmakeKeywords = {
        "STATIC", "SHARED", "MODULE", "INTERFACE", "OBJECT", "ALIAS", "IMPORTED",
        "EXCLUDE_FROM_ALL", "WIN32", "MACOSX_BUNDLE", "GLOBAL"
    };

    for (const QString& cmakePath : cmakeFiles) {
        QFile f(cmakePath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString text = blankCommentsAndStrings(QString::fromUtf8(f.readAll()));
        const QString cmakeDir = QFileInfo(cmakePath).absolutePath();

        static const QRegularExpression cmd(
            QStringLiteral("\\b(add_library|add_executable)\\s*\\("));
        auto it = cmd.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            int end = 0;
            const QStringList args =
                readCommandArgs(text, m.capturedEnd() - 1, &end);
            if (args.isEmpty()) continue;

            ModuleInfo mod;
            mod.name = args.first();
            mod.type = (m.captured(1) == QLatin1String("add_library"))
                           ? QStringLiteral("library") : QStringLiteral("executable");
            mod.relDir = QDir(rootPath).relativeFilePath(cmakeDir);
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
                // Merge sources into the existing target definition.
                ModuleInfo& existing = modules[moduleByName[mod.name]];
                existing.sourceFiles.unite(mod.sourceFiles);
            }
        }

        // target_link_libraries(<target> ... <libs>)  → inter-module deps
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
        const QString rel = QDir(rootPath).relativeFilePath(file);
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
    QHash<QString, int> classByName;                 // class name → index
    QHash<QString, QStringList> classesInFile;       // abs file → class names
    QHash<QString, QStringList> includesInFile;      // abs file → included basenames

    static const QRegularExpression classRe(
        QStringLiteral("\\b(class|struct)\\s+([A-Za-z_]\\w*)\\b"
                       "(?:\\s+final)?\\s*(?::([^{;]*))?\\{"));
    static const QRegularExpression includeRe(
        QStringLiteral("#\\s*include\\s*\"([^\"]+)\""));

    for (const QString& file : cppFiles) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString raw  = QString::fromUtf8(f.readAll());
        const QString code = blankCommentsAndStrings(raw);
        const QStringList rawLines = raw.split(QLatin1Char('\n'));
        const QString relFile = QDir(rootPath).relativeFilePath(file);

        // Local includes (for class → class "uses" edges).
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

            // Brace-match the class body starting at the '{' the regex consumed.
            const int open = m.capturedEnd() - 1;
            int depth = 0, j = open;
            for (; j < code.size(); ++j) {
                if (code[j] == '{') ++depth;
                else if (code[j] == '}') { if (--depth == 0) { ++j; break; } }
            }
            ci.methods = extractMethods(code.mid(open + 1, j - open - 2), ci.name);

            const int declLine = lineOf(code, m.capturedStart());
            const QString lead = leadingComment(rawLines, declLine);
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

    // ── 5. Build the scene ───────────────────────────────────────────────────
    scene->clearAll();

    QHash<QString, GraphNode*> moduleNode;    // module name → node
    QHash<QString, GraphNode*> classNode;     // class name  → node
    QHash<QString, GraphNode*> funcNode;      // "Class::method" → node

    auto place = [](GraphNode* n, int index, qreal y) {
        n->setPos((index % 12) * 180.0, y + (index / 12) * 130.0);
    };

    int mi = 0;
    for (const ModuleInfo& mod : modules) {
        auto* n = new GraphNode(NodeType::Module, mod.name);
        n->setComment(mod.description);
        n->setImplemented(true);
        place(n, mi++, 0.0);
        scene->addItem(n);
        moduleNode.insert(mod.name, n);
    }

    int ci = 0;
    for (const ClassInfo& c : classes) {
        auto* n = new GraphNode(NodeType::Class, c.name);
        n->setComment(c.description);
        n->setImplemented(true);
        place(n, ci++, 320.0);
        scene->addItem(n);
        classNode.insert(c.name, n);
    }

    int fi = 0;
    for (const ClassInfo& c : classes) {
        for (const QString& method : c.methods) {
            const QString qn = c.name + QStringLiteral("::") + method;
            if (funcNode.contains(qn)) continue;
            auto* n = new GraphNode(NodeType::Function, qn);
            n->setComment(QStringLiteral("Member function %1 of %2 %3 (%4).")
                              .arg(qn, c.keyword, c.name, c.relFile));
            n->setImplemented(true);
            place(n, fi++, 640.0);
            scene->addItem(n);
            funcNode.insert(qn, n);
        }
    }

    // ── 6. Edges ─────────────────────────────────────────────────────────────
    auto connect = [scene](GraphNode* a, GraphNode* b, const QString& label) {
        if (a && b && a != b) scene->addItem(new GraphEdge(a, b, label));
    };

    // module → class (contains)
    for (const ClassInfo& c : classes)
        connect(moduleNode.value(c.moduleName), classNode.value(c.name),
                QStringLiteral("contains"));

    // class → function (provides), class → base (inherits), class → class (uses)
    QSet<QString> usesSeen;
    for (const ClassInfo& c : classes) {
        GraphNode* cn = classNode.value(c.name);

        for (const QString& method : c.methods)
            connect(cn, funcNode.value(c.name + QStringLiteral("::") + method),
                    QStringLiteral("provides"));

        for (const QString& base : c.bases)
            if (classNode.contains(base))
                connect(cn, classNode.value(base), QStringLiteral("inherits"));

        // "uses": classes defined in headers this class's file includes.
        for (const QString& inc : includesInFile.value(c.file)) {
            for (const QString& file : classesInFile.keys()) {
                if (QFileInfo(file).fileName() != inc) continue;
                for (const QString& other : classesInFile.value(file)) {
                    if (other == c.name || c.bases.contains(other)) continue;
                    const QString key = c.name + QStringLiteral("->") + other;
                    if (usesSeen.contains(key)) continue;
                    usesSeen.insert(key);
                    connect(cn, classNode.value(other), QStringLiteral("uses"));
                }
            }
        }
    }

    // module → module (depends on), from target_link_libraries
    for (const ModuleInfo& mod : modules)
        for (const QString& lib : mod.linkLibs)
            if (moduleNode.contains(lib))
                connect(moduleNode.value(mod.name), moduleNode.value(lib),
                        QStringLiteral("depends on"));

    return true;
}
