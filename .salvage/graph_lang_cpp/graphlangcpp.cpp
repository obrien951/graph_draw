#include "graphlangcpp.h"
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
            t.remove(QRegularExpression(QStringLiteral("^/\*+|\*+/$|^\*+")));
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
    static const QRegularExpression re(QStringLiteral("(~?[A-Za-z_]\w*)\s*\("));
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
        QStringLiteral("\b(public|protected|private|virtual)\b"));
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
    return args.split(QRegularExpression(QStringLiteral("\s+")), Qt::SkipEmptyParts);
}

} // namespace

// ── Analyze ─────────────────────────────────────────────────────────────────────

bool LanguageAnalyzer::analyze(const ir::RepoFileIndex& index) const
{
    m_lastError.clear();

    // Use the CppAnalyzer stub to perform the actual analysis.
    // The stub will be implemented by the CppAnalyzer node.
    CppAnalyzer analyzer;

    // The stub expects a repo root path. We derive it from the index.
    // The stub's analyze() will return the ir::Repo.
    ir::Repo repo = analyzer.analyze(index.repoRoot());

    if (repo.isEmpty()) {
        m_lastError = analyzer.lastError();
        return false;
    }

    return true;
}

bool LanguageAnalyzer::buildScene(const ir::Repo& repo, GraphScene* scene) const
{
    if (!scene) return false;

    scene->clearAll();

    // Build module nodes
    for (const auto& mod : repo.modules()) {
        auto* n = new GraphNode(NodeType::Module, mod.name);
        n->setComment(mod.description);
        n->setImplemented(true);
        scene->addItem(n);
    }

    // Build class nodes
    for (const auto& cls : repo.classes()) {
        auto* n = new GraphNode(NodeType::Class, cls.name);
        n->setComment(cls.description);
        n->setImplemented(true);
        scene->addItem(n);
    }

    // Build function nodes
    for (const auto& func : repo.functions()) {
        auto* n = new GraphNode(NodeType::Function, func.name);
        n->setComment(func.description);
        n->setImplemented(true);
        scene->addItem(n);
    }

    // Build edges
    for (const auto& edge : repo.edges()) {
        auto* e = new GraphEdge(scene->findNode(edge.source),
                                scene->findNode(edge.target),
                                edge.label);
        scene->addItem(e);
    }

    return true;
}
