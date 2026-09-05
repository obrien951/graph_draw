"""Per-language knobs for a harness that is not C++-only.

The graph, the fence, the walk and the scope audit are all language-neutral.
A handful of things are not, and until now every one of them was hard-wired to
C++/CMake:

* the placeholder idiom shown in the unfilled-call (`HARNESS-STUB`) example and
  the partial-work (`HARNESS-PARTIAL`) example,
* the "a body that calls `Q_UNIMPLEMENTED()`" phrasing in the no-op check,
* the build artefacts (`build/`, `*.o`, `CMakeFiles/`) that must never be
  counted as agent output,
* the verify commands a first-time user is pointed at in the CLI error.

They live here now, keyed by a short language id, with detection from the
repo's own marker files (`Cargo.toml`, `CMakeLists.txt`, `pyproject.toml`, ...)
so a run against a Rust crate usually needs no `--language` flag at all.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

#: Fallback shown when a language has no specific example. Deliberately
#: pseudo-code: better a neutral shape than a C++ one in a Rust brief.
_GENERIC_STUB = (
    "// HARNESS-STUB(<node name>): declared here, implemented by its own node.\n"
    "// Write the call/type exactly as this node needs it, then leave the body\n"
    "// unimplemented in whatever way your language makes an unfinished function\n"
    "// fail loudly (raise / panic / abort) - it must still compile and link."
)
_GENERIC_PARTIAL = (
    "// HARNESS-PARTIAL(<this node>): <one sentence naming the specific blocker>.\n"
    "// Everything else in this node is real, tested and complete; only the\n"
    "// piece this marker sits on is deferred, and the sentence says why."
)


@dataclass(frozen=True)
class Language:
    """Everything the harness needs to phrase a brief for one language."""

    id: str
    aliases: tuple[str, ...] = ()
    #: repo-root-relative glob patterns whose presence identifies the language
    markers: tuple[str, ...] = ()
    #: extra ignore globs unioned into the scope config's ignore list, so build
    #: output never looks like agent changes and never trips the fence
    ignore: tuple[str, ...] = ()
    #: how an unfinished function fails loudly in this language, for the no-op
    #: check ("... a body that <phrase>, or any TODO ... is NEVER a no-op")
    unfinished_phrase: str = "raises / panics / aborts"
    #: verify commands suggested in the CLI error when none are configured
    verify_hint: tuple[str, ...] = ()
    stub_example: str = _GENERIC_STUB
    partial_example: str = _GENERIC_PARTIAL
    #: source suffixes worth pulling in as neighbour context for a node
    source_suffixes: tuple[str, ...] = ()


CPP = Language(
    id="c++",
    aliases=("cpp", "cxx", "cc"),
    markers=("CMakeLists.txt", "*.cmake", "Makefile", "meson.build", "*.vcxproj"),
    ignore=(
        "build/**", "cmake-build-*/**", "**/CMakeFiles/**", ".qt/**",
        "**/*.o", "**/*.a", "**/*.so", "**/*.dylib", "**/Makefile",
        "**/cmake_install.cmake", "**/CTestTestfile.cmake", "**/compile_commands.json",
    ),
    unfinished_phrase="throws / returns a placeholder / calls Q_UNIMPLEMENTED()",
    verify_hint=(
        "cmake -S . -B build",
        "cmake --build build -j4",
        "cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure",
    ),
    stub_example=(
        "// HARNESS-STUB(ir::Repo): declared here, implemented by its own node.\n"
        "ir::Repo RustAnalyzer::analyze(const RepoFileIndex& index) const {\n"
        "    Q_UNIMPLEMENTED();   // or: throw std::logic_error(\"HARNESS-STUB(...)\");\n"
        "    return {};\n"
        "}"
    ),
    partial_example=(
        "// HARNESS-PARTIAL(RustAnalyzer::analyze): crate dependency edges are not\n"
        "// resolved here - that needs TomlDocument::parseText, a separate, not-yet-\n"
        "// built node, and guessing its output shape here would commit to a contract\n"
        "// it might not honour. Everything else below is real, tested and complete.\n"
        "ir::Repo RustAnalyzer::analyze(const RepoFileIndex& index) const {\n"
        "    ir::Repo repo = scanCratesAndModules(index);   // real work, done\n"
        "    return repo;   // dependency edges intentionally left out - see marker above\n"
        "}"
    ),
    source_suffixes=(".h", ".hpp", ".hh", ".cpp", ".cxx", ".cc", ".c", ".txt", ".cmake"),
)

RUST = Language(
    id="rust",
    aliases=("rs", "cargo"),
    markers=("Cargo.toml", "*/Cargo.toml"),
    ignore=("target/**", "**/*.rs.bk", "Cargo.lock"),
    unfinished_phrase="panics / calls todo!() / unimplemented!() / returns a placeholder",
    verify_hint=("cargo build --all-targets", "cargo test"),
    stub_example=(
        "// HARNESS-STUB(scan_rust_file): implemented by its own node.\n"
        "pub fn scan_rust_file(text: &str) -> RustFileItems {\n"
        "    unimplemented!(\"HARNESS-STUB(scan_rust_file)\")\n"
        "}"
    ),
    partial_example=(
        "// HARNESS-PARTIAL(scan_rust_file): multi-line string literals inside a\n"
        "// scanned file are not handled; the brace-depth scanner would need a\n"
        "// dedicated string-aware pass this turn didn't have room for.\n"
        "pub fn scan_rust_file(text: &str) -> RustFileItems {\n"
        "    // real, working scan for everything except multi-line strings\n"
        "}"
    ),
    source_suffixes=(".rs", ".toml"),
)

PYTHON = Language(
    id="python",
    aliases=("py",),
    markers=("pyproject.toml", "setup.py", "setup.cfg", "requirements*.txt"),
    ignore=("**/__pycache__/**", "**/*.pyc", ".venv/**", "**/*.egg-info/**", ".pytest_cache/**"),
    unfinished_phrase="raises NotImplementedError / returns a placeholder / has a bare `...` body",
    verify_hint=("python -m pytest -q",),
    stub_example=(
        "def merge(curated, generated):\n"
        "    # HARNESS-STUB(GraphMerger::merge): implemented by its own node.\n"
        "    raise NotImplementedError(\"HARNESS-STUB(GraphMerger::merge)\")"
    ),
    partial_example=(
        "def merge(curated, generated):\n"
        "    # HARNESS-PARTIAL(GraphMerger::merge): only additive merging is done;\n"
        "    # stale-node pruning needs a comparison this turn ran out of budget for.\n"
        "    ...  # the real, working partial logic goes here, not a stub"
    ),
    source_suffixes=(".py", ".pyi", ".toml"),
)

TYPESCRIPT = Language(
    id="typescript",
    aliases=("ts", "javascript", "js", "node"),
    markers=("tsconfig.json", "package.json"),
    ignore=("node_modules/**", "dist/**", "build/**", "**/*.tsbuildinfo", "coverage/**"),
    unfinished_phrase="throws / returns a placeholder / has a `// TODO` body",
    verify_hint=("npm run build", "npm test"),
    stub_example=(
        "// HARNESS-STUB(scanFile): implemented by its own node.\n"
        "export function scanFile(text: string): FileItems {\n"
        "  throw new Error('HARNESS-STUB(scanFile)');\n"
        "}"
    ),
    partial_example=(
        "// HARNESS-PARTIAL(scanFile): multi-line template literals are not handled\n"
        "// yet; the scanner needs a dedicated pass this turn had no room for.\n"
        "export function scanFile(text: string): FileItems {\n"
        "  // real, working scan for everything else\n"
        "}"
    ),
    source_suffixes=(".ts", ".tsx", ".js", ".jsx", ".json"),
)

#: A neutral entry so an unknown --language never crashes the run.
GENERIC = Language(id="generic")

_ALL = (CPP, RUST, PYTHON, TYPESCRIPT)
_BY_KEY: dict[str, Language] = {}
for _lang in _ALL:
    _BY_KEY[_lang.id] = _lang
    for _alias in _lang.aliases:
        _BY_KEY[_alias] = _lang

#: The default when nothing is configured and nothing detected. C++ keeps the
#: harness's historical behaviour for graph_draw's own Part B.
DEFAULT = "c++"


def get(name: str | None) -> Language:
    """The Language for an id or alias; GENERIC for anything unrecognised."""
    if not name:
        return _BY_KEY[DEFAULT]
    return _BY_KEY.get(name.strip().lower(), GENERIC)


def known(name: str | None) -> bool:
    return bool(name) and name.strip().lower() in _BY_KEY


def choices() -> list[str]:
    """Ids plus aliases, for argparse `choices=`."""
    return sorted(_BY_KEY)


def detect(root: str | Path, files: Sequence[str] | None = None) -> str | None:
    """The language id implied by the repo's marker files, or None.

    ``files`` may be a pre-listed set of repo-relative paths (the harness
    already has one from git); without it the repo root is scanned shallowly.
    Rust is checked before C++ because a Rust crate that vendors a C
    dependency can carry a stray ``Makefile`` while ``Cargo.toml`` is the
    real signal.
    """
    from .pathglob import matches_any

    root = Path(root)
    if files is None:
        names: list[str] = []
        try:
            for p in root.iterdir():
                names.append(p.name)
                if p.is_dir() and not p.name.startswith("."):
                    names += [f"{p.name}/{c.name}" for c in _safe_iterdir(p)]
        except OSError:
            names = []
    else:
        names = list(files)

    for lang in (RUST, CPP, PYTHON, TYPESCRIPT):
        if any(matches_any(lang.markers, name) for name in names):
            return lang.id
    return None


def _safe_iterdir(d: Path) -> list[Path]:
    try:
        return list(d.iterdir())
    except OSError:
        return []


def resolve(explicit: str | None, configured: str | None,
            root: str | Path, files: Sequence[str] | None = None) -> str:
    """--language wins, then the scope config's `language`, then detection,
    then the historical default. Always returns a non-empty id."""
    for candidate in (explicit, configured, detect(root, files)):
        if candidate:
            return candidate.strip().lower()
    return DEFAULT
