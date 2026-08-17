# Baby Feed Tracker (Rust) + Rust support in graph_draw

> **Resume the session that produced this:**
> ```
> claude --resume 3cedc4d2-db1d-4292-8004-0a7713e94e48
> ```
> Run from `/Users/josephsenanobrien/repos/graph_draw`.
> Nothing in Parts A/B/C has been built yet. The only artifact produced so far is
> `graphs/graph_draw.planned.json` — see [The planned graph](#the-planned-graph).

---

## Context

Two deliverables, in two repos:

1. **A new app** — a baby feeding tracker for me and my wife, both on Samsung Android
   phones, talking over Tailscale to a server on my home desktop. Backend in Rust, and as
   much of the frontend in Rust as is actually possible.
2. **Changes to `graph_draw`** (this repo) so it can draw the dependency graph of that Rust
   app *post-facto*, the way `repo_to_graph` already does for C++ packages.

`graph_draw`'s graphs are implementation plans, not just pictures: each node's `comment` is
the prompt used to implement it, and `implemented` tracks whether it exists
(`memory/project_graphs_as_implementation_plans.md`). Today the analyzer is C++-only. The
new app is the first non-C++ target and the first real test of whether the graph format
survives a second language.

A third question falls out of that and gets its own section: **when an AI reads a codebase
and draws the graph by hand, how does that compare to what the heuristic scanner produces?**
That comparison is the real measure of whether `repo_to_graph` is good enough.

### Decisions

| | |
|---|---|
| Frontend | Rust/WASM PWA (Leptos CSR), served by the Rust server over Tailscale HTTPS |
| Offline | Local-first: log instantly to local storage, replay an outbox when reachable |
| graph_draw | Extract a `LanguageAnalyzer` plugin seam, then add a Rust analyzer |
| Node kinds | Keep the existing three. Do **not** add `Trait`/`Endpoint` |
| Notifications | Not in v1 |

---

## Question 1: Is a Rust-only architecture possible?

**Yes — with one unavoidable exception, and it is small.**

The backend isn't in question: axum + sqlx + SQLite is a boring, mature Rust stack.

The frontend is the interesting half, and **Leptos compiled to WASM and delivered as an
installable PWA gets there.** UI components, reactive state, routing, local persistence, the
sync engine, and all shared domain types are Rust. The non-Rust surface of the entire system:

- `index.html` — ~30 lines, mostly a Trunk manifest
- `sw.js` — the service worker, ~60 lines. **Service workers must be JavaScript**; the
  browser will not register a WASM module as one. You *can* make it a 3-line loader that
  imports a WASM bundle, but for cache-and-replay logic that's a bad trade. Write the JS.
- `manifest.webmanifest` — JSON
- some CSS

No Kotlin, no Java, no Gradle, no NDK, no APK signing, no Play Store.

**Why the PWA rather than a native APK:** Tailscale is already in use, and `tailscale serve`
terminates HTTPS with a real Let's Encrypt certificate on a `*.ts.net` name. That HTTPS
origin unlocks everything — service workers, installability, and add-to-home-screen all
require a secure context. It also injects `Tailscale-User-Login` / `Tailscale-User-Name`
identity headers and strips client-supplied copies to prevent spoofing, which is auth for
free ([docs](https://tailscale.com/kb/1312/serve)).

**Escape hatch:** Tauri 2 supports Android and hosts the *same* Leptos WASM frontend in its
webview. Nothing in A1–A11 would be rewritten; add a shell crate. That's the reason to
prefer Leptos-CSR over a server-rendered setup — it keeps the door open.

### The one hard architectural constraint

The server must sit behind `tailscale serve`, not bind directly to the tailnet IP. Binding
axum to `http://100.x.y.z:8080` gives an insecure origin: **no service worker, no install
prompt, no offline.**

### The caveat to settle before building anything

Installing a PWA as a true chrome-less app on Android relies on Chrome's **WebAPK minting
service**, which runs on Google's servers and fetches the manifest itself. It cannot reach a
private `*.ts.net` name. When minting fails Chrome falls back to a plain home-screen
shortcut, and there are reports of exactly this on private networks — `display: standalone`
ignored, app opens with Chrome's UI visible
([chromium-discuss](https://groups.google.com/a/chromium.org/g/chromium-discuss/c/ebh9p7M7P5o)).

**Not at risk:** service worker, offline caching, local storage, and the home-screen icon
depend only on the secure origin and work fine over Tailscale — there's a
[documented working setup](https://github.com/stasstepv/hermes-pwa/blob/main/docs/NETWORK_TAILSCALE.md).
Only the cosmetic standalone launch is uncertain.

**If standalone matters**, in order of preference: enable Tailscale Funnel briefly *just*
during install so the minting server can fetch the manifest, then turn it off (weigh the
short public exposure); or try Samsung Internet, which mints its own WebAPKs on Samsung
devices; or take the Tauri 2 hatch. **Task A0 settles this on day one.**

### Verified environment

`rustc 1.96.0` present; Android SDK+NDK present (unused on this path); `node`, `cmake`, Qt6
present. **Missing:** `rustup target add wasm32-unknown-unknown`, `cargo install trunk`, and
Tailscale is not installed on this Mac (only the desktop and phones strictly need it, but
it's wanted here for testing).

Stable versions checked against crates.io: `axum 0.8.9`, `sqlx 0.9.0`, `tokio 1.53`,
`uuid 1.24`, `tower-http 0.7.0`, `leptos 0.8.20`, `trunk 0.21.14`. Pin these — `leptos 0.9`
and `trunk 0.22` are prerelease.

---

## Question 2: What must change in graph_draw?

The model layer is already language-neutral — `NodeType {Module, Class, Function}`
(`graph_core/graphnode.h:8`), the five edge labels, and the JSON format carry no C++
specifics. **All C++ knowledge is confined to `graph_analyze/repoanalyzer.cpp`.**

The problem is there's no seam. `RepoAnalyzer::analyze()` (`repoanalyzer.cpp:255-561`) is a
~300-line monolith that walks files, parses CMake, regexes classes, and builds the scene in
one function. Its last two phases (`:469-558`) are nearly pure functions of two file-local
structs, `ModuleInfo`/`ClassInfo` (`:19-37`) — that's the natural seam.

So the work is **more refactor than parser**. Rust is easier to scan than C++ (an explicit
`fn` keyword replaces the entire `extractMethods` heuristic at `:168-206`), and it maps onto
the existing three kinds cleanly:

| Rust | graph_draw |
|---|---|
| crate (`Cargo.toml` `[package]`) | `Module` |
| `src/<name>.rs` or `src/<name>/` | nested `Module`, "contains" |
| intra-workspace `[dependencies]` | `module → module` "depends on" |
| `struct` / `enum` / `trait` / `union` | `Class` |
| `fn` in an `impl` or `trait` body | `Function`, named `Type::method` |
| free `fn` at module level | `Function`, attached to Module via "contains" |
| `impl Trait for Type`, `trait A: B` | `class → class` "inherits" |
| `use` paths | `class → class` "uses" |
| `///` and `//!` doc comments | node `comment` (the implementation prompt) |

### Findings from reading the code that change the shape of the work

1. **`tests/test_repoanalyzer.cpp` is untracked.** The regression baseline isn't in git.
   Commit it before touching anything (B0). Verified green: **11 test cases, 96 assertions**,
   4 tagged `[repoanalyzer]`.
2. **`QDirIterator` (`:270`) descends into skipped dirs and filters afterwards.** Merely
   wasteful for C++; fatal for Rust — a warm `target/` is tens of thousands of files.
3. **The "uses" loop iterates `QHash` keys (`:540`)**, so edge *array order* is
   nondeterministic between runs. The edge set is stable, so B3's acceptance gate must
   compare node/edge **sets**, not a byte diff.
4. **Node labels clip.** `graphnode.cpp:261` word-wraps into a 140×65 box with no eliding,
   and `::` isn't a wrap point — `backend::routes::create_user` renders as mush. Hence the
   shortest-unique-label resolver in B8.
5. **`inherits` is never actually emitted for this repo** — `:535` requires the base to be a
   known node. The same guard on Rust traits makes `#[derive(Serialize)]` a non-issue free.
6. **`graph_analyze` links `graph_io` PUBLIC but never uses it.** Drop it.
7. **Three genuinely new components:** a TOML reader (Qt has none), a Rust-aware lexer, and
   a merge mode.

### Why a merge mode is the piece that matters most

Every generated node is hardcoded `implemented: true` (`:484, :495, :508`) and positions
come from a fresh grid (`:476-478`), so regenerating over a curated plan destroys both.
Since these graphs *are* the implementation plans, merge mode is what makes the tool usable
more than once. Rules in B9.

---

# Part A — The app (`babytrack`, new repo)

```
Phone (Chrome PWA)                    Desktop
┌──────────────────────┐             ┌────────────────────────────┐
│ Leptos CSR → WASM    │             │ tailscale serve (HTTPS,    │
│ local store + outbox │──tailnet───▶│   LE cert, identity hdrs)  │
│ service worker cache │             │   └─▶ axum :8080 (loopback)│
└──────────────────────┘             │        ├─ /api/*  sync     │
                                     │        └─ /  ServeDir dist │
                                     │      SQLite (WAL)          │
                                     └────────────────────────────┘
```

One binary serves both API and frontend. One process to run, one file to back up.

## Workspace layout

Mirrors the multi-library / thin-integration-root preference
(`memory/feedback_project_structure.md`) — and gives the graph something worth drawing.

```
babytrack/
  Cargo.toml              [workspace] members = ["crates/*"]
  crates/
    bt-types/             Event, EventKind, payloads, sync DTOs. wasm + native.
    bt-core/              Pure domain: merge/LWW, stats, validation. No I/O.
    bt-store/             sqlx SQLite; impls bt-core's EventStore trait.
    bt-server/            axum wiring + auth + static serving. Thin main.rs.
    bt-web/               Leptos CSR, Trunk, index.html, sw.js, manifest, icons.
  migrations/
  graphs/                 repo_to_graph output lands here
```

`bt-core` defining an `EventStore` trait that `bt-store` implements is deliberate: it gives
the Rust analyzer an `impl Trait for Type` to turn into an "inherits" edge.

## Data model and sync

Client-generated UUIDv7 ids make every write idempotent, so retrying the outbox is free. The
server owns a monotonic `seq` used as the sync cursor — never wall-clock time, which would
break on clock skew between two phones.

```rust
struct Event {
    id: Uuid,                    // v7, client-generated
    kind: EventKind,             // Bottle | Nursing | Diaper | Sleep | Note
    started_at: OffsetDateTime,
    ended_at: Option<OffsetDateTime>,
    payload: Payload,            // serde-tagged per kind
    created_by: String,          // from Tailscale identity header
    revision: OffsetDateTime,    // client edit clock, for last-write-wins
    deleted: bool,               // soft delete, so deletions replicate
    seq: i64,                    // server-assigned, sync cursor
}
```

- `POST /api/events` — batch upsert by id; LWW on `revision`; returns new seqs.
- `GET /api/events?since=<seq>` — everything changed since the cursor, plus the new cursor.
- `GET /api/health` — liveness, for the connectivity indicator.

Client flow: write locally → render immediately → append id to outbox → fire sync. Sync =
POST the outbox, GET since cursor, merge LWW. The merge function lives in `bt-core` and is
used verbatim by both sides, so it's unit-tested once.

Local storage is `localStorage` with a serde-serialized event list, not IndexedDB. A few
thousand feed events is well under the 5 MB budget, per the safety-over-optimization
preference (`memory/feedback_safety_over_perf.md`). Revisit only if it actually gets big.

## Tasks — each ≈2–8 h human dev / 50k–100k agent tokens

### A0 — Spike: prove the Tailscale HTTPS + PWA path *(do this first)*
The only genuinely uncertain part of the plan. On the desktop: install Tailscale, run
`tailscale serve` in front of a hello-world server with a manifest and a trivial service
worker; confirm a real cert. On a phone: confirm the service worker registers and activates,
install it, and **check specifically whether it launches chrome-less or with Chrome's UI
visible**. Capture the actual identity headers arriving at the backend.
**Done when:** three facts are known — service worker activates (y/n), install gives
standalone or shortcut, and the exact headers.
**Decides:** A5's auth mechanism and whether A12 targets standalone. If standalone is a
dealbreaker and no mitigation works, switch to the Tauri 2 shell *before A6*, at which point
almost nothing is wasted.

### A1 — Workspace skeleton + `bt-types`
All five crates, stubbed (stub and link even before they do anything). Define `Event`,
`EventKind`, `Payload`, sync DTOs. Compile `bt-types` for **both**
`wasm32-unknown-unknown` and native.
**The classic first-day wasm papercut:** UUIDv7 in the browser needs `getrandom`'s `wasm_js`
feature — as of getrandom 0.4 this is a plain Cargo feature; guides saying to set
`RUSTFLAGS --cfg getrandom_backend="wasm_js"` are stale (that was 0.3). Enable it in the
`bt-web` binary, **not** in `bt-types` — the docs warn against libraries turning it on.
**Tests:** serde round-trip for every `EventKind`; confirm the wasm target builds.

### A2 — `bt-store`: schema, migrations, persistence
sqlx + SQLite in WAL mode. `events` keyed by `id` with `AUTOINCREMENT seq`, indexed on `seq`
and `started_at`. Upsert-by-id, `changed_since(seq)`, soft delete.
**Tests:** temp-file DB — upsert idempotent, `seq` monotonic, `changed_since` never misses.

### A3 — `bt-core`: domain rules and the merge algorithm
Pure, no I/O: the `EventStore` trait, LWW merge, validation, derived stats (time since last
feed, 24h totals, average interval, per-day rollups).
**Tests:** the crate that deserves real coverage. Merge is commutative and idempotent;
concurrent edits from two devices converge; a tombstone always beats an older edit.

### A4 — `bt-server`: axum wiring
Sync endpoints, `/api/health`, `ServeDir` with index.html fallback for SPA routes, env
config, `tracing`, graceful shutdown.
**Tests:** endpoint tests over the real router with an in-memory store.

### A5 — Auth
An axum extractor reading the Tailscale identity headers captured in A0, populating
`created_by`. Shared-secret bearer token as fallback and for local dev.
**Bind the listener to loopback** so `tailscale serve` is the only network path in —
otherwise anything on the tailnet can hit :8080 and set the header itself. Tailscale strips
client-supplied identity headers, so the proxied path is trustworthy; a process on the
desktop itself could still spoof over loopback, which is fine for a home machine but worth
knowing has been accepted.

### A6 — `bt-web` skeleton: Leptos + Trunk
Leptos 0.8 CSR, `index.html`, Trunk build, dev proxy to the backend. Renders a page calling
`/api/health`.
**Done when:** `trunk build --release` produces a `dist/` that `bt-server` serves and the
health check goes green on the phone.

### A7 — Client sync engine
Local store over `localStorage`, outbox, cursor tracking, background sync on interval and on
regaining connectivity, plus a visible online/offline/pending indicator. Reuses `bt-core`'s
merge.
**Tests:** wasm-bindgen-test for the store; merge paths covered by `bt-core`'s native tests.

### A8 — Home screen
The screen actually used at 3am. Big one-thumb buttons: Bottle, Left, Right, Diaper.
Prominent, live-updating "last feed: 2h 14m ago · 120 ml". Optimistic logging with undo.

### A9 — Nursing timer
Start/stop per side, persisted so backgrounding or locking the phone doesn't lose the
session; resumes on reopen. Handles "started left, switched to right".

### A10 — History
Day-grouped list, tap to edit, delete with confirmation, and backfill an event forgotten at
the time (manual time picker — this matters more than it sounds).

### A11 — Stats
Hand-rolled inline SVG: feeds/day, volume/day, interval distribution. No JS charting
dependency; keeps the frontend pure Rust.

### A12 — PWA shell
`manifest.webmanifest`, 192/512 icons, and `sw.js`: precache the app shell, cache-first for
assets, network-first for `/api/`. Trunk emits hash-named `.wasm`/`.js`, so **generate the
precache list in a Trunk post-build hook** rather than hardcoding filenames — this is the
part people get wrong and then serve a stale WASM bundle forever.
**Done when:** airplane mode still opens the app, shows history, and accepts a new feed that
syncs once back on the tailnet.

### A13 — Night-use polish
Dark theme by default, large hit targets, one-handed reach, minimal brightness, no layout
shift when numbers update. Test it in an actually dark room.

### A14 — Deployment
`tailscale serve` config, a service unit that survives reboot (systemd if the desktop is
Linux — adapt otherwise), nightly SQLite backup via `VACUUM INTO`, one-command release
build. **Assumption to confirm: desktop OS.** Only this task changes.

### A15 — Real-world shakedown
Both phones installed. Log concurrently, log offline, edit the same event from both, kill
the server mid-write, reboot the desktop. Budget real time — this is where sync bugs surface.

---

# Part B — graph_draw changes

## Target architecture

Five libraries, strictly acyclic, one per subsystem:

```
graph_lang            (Qt Core only)   IR + LanguageAnalyzer ABC + file index + lexing + label resolver
   ↑            ↑
graph_lang_cpp   graph_lang_rust       one library per language plugin
   ↑            ↑
graph_analyze        (+ graph_core)    GraphBuilder (IR → GraphScene) + RepoAnalyzer (dispatch)
   ↑
repo_to_graph        (+ graph_io, graph_merge)
```

`graph_merge` is independent (needs `graph_core` only). Keeping `graph_lang*` free of
`graph_core` mechanically enforces "analyzers produce data, never scene items" and lets
their tests run without a `QApplication`.

```
graph_lang/       languageir.h  languageanalyzer.h  repofileindex.*  sourcetext.*  labelresolver.*
graph_lang_cpp/   cppanalyzer.*
graph_lang_rust/  cargotoml.*  rustlexer.*  rustsyntax.*  rustanalyzer.*
graph_analyze/    graphbuilder.*  repoanalyzer.*   (path + class name unchanged)
graph_merge/      graphmerger.*
```

`repoanalyzer.h` keeps its path and class name, so `analyze/main.cpp` and
`tests/test_repoanalyzer.cpp` need no include changes.

## The IR

```cpp
namespace ir {
struct Function { QString label; QString description; };
struct Type {                       // C++ class/struct, Rust struct/enum/trait
    QString label, description, moduleLabel;
    QStringList baseLabels, usesLabels;
    QList<Function> methods;
};
struct Module {                     // CMake target/dir, Cargo crate, Rust module
    QString label, description;
    QStringList dependsOnLabels, childModuleLabels;
    QList<Function> functions;
};
struct Repo { QList<Module> modules; QList<Type> types; };
}
```

Two decisions that matter: **all edge endpoints are labels, not indices** — the builder
resolves against the node tables and silently drops unresolved ones, reproducing
`if (classNode.contains(base))` (`:535`) and `if (moduleNode.contains(lib))` (`:556`) without
special-casing. And **descriptions live in the IR**, so the builder never formats text —
today the function description is built inside the scene phase (`:506-507`), which is exactly
why `ModuleInfo`/`ClassInfo` aren't quite language-neutral as they stand.

Edge emission order: module→module "contains", module→class "contains", module→function
"contains", class→function "provides", class→class "inherits", class→class "uses",
module→module "depends on". **No new edge labels** — free Rust functions reuse "contains".

## Tasks

**B0 — commit the baseline** · 15 min · *blocking*
`git add tests/test_repoanalyzer.cpp graphs/ memory/project_graphs_as_implementation_plans.md`
and commit. The regression contract must exist in history before anything moves.

**B1 — `graph_lang` foundation: file index + shared lexing** · 3–4 h · *parallel*
Move `blankCommentsAndStrings` → `sourcetext::blankC` **unmodified**, plus `leadingComment`,
`lineOf`, and a new `matchingBrace` (factors out the identical loops at `:435-438`).
`isSkippedDir` → `repofileindex.cpp`; rewrite the walk as a non-descending recursion with
per-directory sorted entries. **Add a `CACHEDIR.TAG` check** — Cargo writes
`target/CACHEDIR.TAG`, killing the `target/` problem language-neutrally. Preserve the
existing quirk that the skip predicate also applies to the file's own basename.
**Make the length-preservation invariant explicit and tested** (`blankC(s).size() == s.size()`)
— load-bearing today but only implicit, and the Rust lexer must satisfy it too.

**B2 — IR + `GraphBuilder`** · 3–4 h · needs B1
Given a hand-built `ir::Repo`, produce the expected nodes and exactly the seven edge
categories; unresolved labels produce no edge and no crash; duplicates dropped after the
first; self-edges suppressed. Factor `hasEdge`/`nodeNamed` helpers into a shared
`tests/scene_helpers.h` — but leave `test_repoanalyzer.cpp` itself untouched.

**B3 — `CppAnalyzer` extraction + `RepoAnalyzer` dispatch** · 5–6 h · needs B2
**The riskiest task — pure motion only, no "while I'm here" edits.** Move phases 2–4
verbatim; move the `usesLabels` computation analyzer-side with sorted iteration (fixes the
nondeterminism); add `setLanguage`/`languagesUsed`/`analyzeToIr`.
**Acceptance:** `test_repoanalyzer.cpp` passes with **zero edits**; `repo_to_graph .` yields
a node set and edge set identical to committed `graphs/graph_draw.generated.json`, compared
as **sorted multisets** — not a text diff, excluding positions (finding 3);
`repoanalyzer.cpp` under ~120 lines; `graph_analyze` no longer links `graph_io`.

**B4 — Rust lexer** · 3–4 h · *parallel* (needs only B1's headers)
A forward scanner, not an enum state machine — raw strings need lookahead and block comments
nest. Write a *second* function; leave the C++ one byte-identical.
- nested `/* /* */ */` via depth counter
- raw strings `r"…"`, `r#"…"#`, `r##"…"##` — match the hash count
- **raw identifiers `r#type`, `r#match`** — `r#` *not* followed by `"` is an ordinary
  identifier. **This is the trap**: naive detection swallows the rest of the file.
- byte forms `b"…"`, `br#"…"#`, `b'x'`, guarded so the prefix isn't preceded by an ident char
- **the lifetime/char ambiguity**: `'` opens a char literal iff next is `\`, or the char two
  ahead is `'`, or it's a surrogate pair. Otherwise it's a lifetime or loop label (`'a`,
  `'static`, `'_`, `'outer:`) emitted verbatim. Handles `&'a str`, `impl<'a>`,
  `'outer: loop {`, `'\n'`, `'\u{1F600}'`.
- `leadingDoc` must **skip `#[...]` attribute lines** — the dominant real shape is a `///`
  line, then `#[derive(Debug, Serialize)]`, then the item. Strip 3 chars for `///` and `//!`,
  not 2; today's `t.remove(0, 2)` (`:126`) leaves a stray `/`.

**B5 — TOML reader** · 5–6 h · *parallel* (needs nothing but Qt)
`TomlDocument`, read-only, flattened to dotted paths (`package.name`, `bin[1].name`,
`dependencies.sqlx.version`). Insertion-ordered so `keys()` follows file order and output
stays deterministic.
**Must handle:** `[section]`, dotted and **quoted** headers
(`[target.'cfg(target_arch = "wasm32")'.dependencies]` must not split on the inner dot),
`[[array-of-tables]]`, scalars, multi-line arrays with comments and trailing commas, inline
tables, dotted keys (`serde.workspace = true`), `#` comments, `\"`/`\\`, and
`[dependencies.leptos]` sub-tables. `"""…"""` must be **skipped over** without
desynchronizing the tables that follow.
**Must not handle:** writing, TOML's type system (dates, floats, `1_000`, hex), nested inline
tables, full escape decoding, validation. Malformed lines are skipped, not fatal — a partial
manifest read still produces a usable graph.
Highest value-per-hour test file in the plan: pure, no Qt GUI needed.

**B6 — `RustAnalyzer` part 1: crates, modules, deps** · 4–6 h · needs B2, B5
Crate discovery from `[workspace] members` (expanding `/*` globs, honoring `exclude`) ∪ every
`Cargo.toml`, deduped by directory, **sorted by crate name**. Module per crate, label =
`package.name` — the name that appears in `use` statements and other crates'
`[dependencies]`, making edge resolution trivial. `[lib]` never creates a second module;
`[[bin]]` does only when its name differs from the package name.
**Nested modules, depth-1 only, derived from the filesystem:** each `src/<name>.rs` or
`src/<name>/` becomes a Module with a `crate → module` "contains" edge. `backend::routes` and
`backend::db` are exactly the granularity at which a person writes a plan item, and deriving
from paths means no `mod` resolution and no chance of self-disagreement.
Dependencies: `keys("dependencies")` ∪ dev ∪ build, kept only if they name another discovered
crate (normalize `-`↔`_`) or have a `path` key. Consult `[workspace.dependencies]` too.
**Tests:** a `buildSampleCargoWorkspace` fixture in the style of `buildSampleRepo`
(`test_repoanalyzer.cpp:46-81`), including a decoy `target/` with `CACHEDIR.TAG` and a `.rs`
file that must not appear.

**B7 — `RustAnalyzer` part 2: types, methods, edges** · 6–8 h · needs B4, B6
A **single brace-depth scanner**, not global regex matches — that's what keeps `impl` blocks
inside `macro_rules!` and `fn`s inside function bodies out of the graph. Items recognized
only at item depth, applied to blanked text so braces in strings can't corrupt the depth.
- `impl` headers need a real scan, not a regex: find the first `{` not nested in `<>`/`(`/`[`,
  strip leading generics, split on a depth-0 ` for `, strip trailing `where`. A regex breaks
  on `where` clauses and `Vec<Box<dyn T>>`.
- methods: `\bfn\s+([A-Za-z_]\w*)` at depth 1 — covers `pub async unsafe fn` and
  `extern "C" fn` with no prefix matching, and function-pointer types `fn(u32)` don't match.
- `use` expansion of `{a, b::c, d as e}` groups, aliases resolved to the pre-alias path.
- "uses" resolution is **file-granular**, mirroring the C++ include heuristic's fidelity.
  Only repo-defined types resolve; `axum`/`serde`/`std` produce nothing.
**Tests:** trait with required + provided methods; `#[derive]` between doc comment and item
(asserting the doc still lands in `comment`); `impl Repository for PgUsers`; an axum-style
`pub async fn create_user(State(s): State<AppState>) -> impl IntoResponse`; a nested `fn`
inside a function body that must *not* appear.

**B8 — label resolution + description polish** · 2–3 h · needs B7
Because labels clip at ~2 short lines (finding 4), crate-qualifying everything costs real
readability, but not qualifying lets `Error`/`Config`/`AppState` collide across crates — and
the builder would silently drop the second. A two-pass `LabelResolver` picks the shortest
unambiguous candidate: `AppState` stays bare, two `Error`s become `shared::Error` and
`backend::Error`. Method labels inherit their type's resolved label.

**B9 — `GraphMerger`** · 3–4 h · *parallel* (needs only `graph_core`; can start at B0)
Graph-to-graph, file-free, so it's testable with two in-memory scenes. Match by
`(kind, label)`:
- **in both** → keep curated comment *(unless empty — then fill from generated, so re-running
  enriches placeholder nodes)* and curated position; set `implemented = true`
- **generated only** → added, laid out **below the curated bounding box**, not on top of it
- **curated only** → kept, forced `implemented = false` — planned but not built
- **edges purely additive**, unioned by (source, target, label); hand-drawn edges the
  analyzer can't infer are never lost. Pruning would need an explicit flag; out of scope.

This is the whole point: after a merge, `GraphScene::readyToImplement()` lists exactly the
planned-but-unbuilt nodes and `blocked()` lists those still waiting.

**B10 — CLI** · 2–3 h · needs B3, B6, B9
`repo_to_graph [--lang cpp|rust] [--merge <existing.json>] [--dry-run] <repo-root> [output.json]`,
via `QCommandLineParser` for free `--help`. **The default output path stays
`<root>/graphs/<name>.generated.json` and `--merge` never writes to its own input** unless
that path is passed explicitly — `memory/project_graphs_as_implementation_plans.md` records
that this tool once overwrote the curated `graph_draw.json`. Summary gains
`kept N · added M · stale K`. `--dry-run` reports those stats without writing, which is what
Part C uses as its comparison harness. Polyglot repos run both analyzers into one `ir::Repo`,
deduped first-wins; `--lang` is the escape hatch.

**B11 — docs and the self-describing graph** · 2 h · needs all
Update `repoanalyzer.h:6-24` (currently states C++-only), `analyze/main.cpp:10-17`,
`graphs/README.md`, and the memory notes. Reconcile `graphs/graph_draw.planned.json` against
what actually got built — it is the plan, so where reality diverged, the divergence is the
interesting artifact.

**B12 — optional polish** · 2–3 h · *parallel, independent*
(a) Dynamic layout bands in `GraphBuilder` — the fixed y-bands overlap once a band exceeds
~36 nodes. (b) Elide long labels via `QFontMetrics::elidedText`. (c) Two **pre-existing** C++
lexer bugs worth fixing while in there: digit separators (`1'000'000` enters `Chr` state and
blanks the rest of the line) and C++11 raw strings `R"(he said "hi")"`.

**B13 — AI-drawn babytrack graph** · needs Part A ≈A11
**This is an AI task, not a hand-curation task.** Point an agent at the finished `babytrack`
workspace with no access to `repo_to_graph` output, and have it read the source and emit
`graphs/babytrack.ai.json` directly in the `graph_io` format. The prompt should ask for the
graph a competent engineer would draw as an *implementation plan* — the architecture as
intended, not merely the symbols present. Specifically:
- choose the right altitude: not every `fn`, but every piece someone would plan separately
- `comment` must be a genuine implementation prompt (what to build and why), matching the
  voice of `graphs/graph_draw.json`, not `graph_draw.generated.json`'s
  "Member function X of class Y" boilerplate
- capture edges the parser structurally cannot see: that `bt-store` exists to satisfy
  `bt-core`'s `EventStore` seam, that `bt-web`'s outbox mirrors `bt-core`'s merge
- mark everything `implemented: true`, since it reflects code that exists
**Validate mechanically** before accepting: kinds ∈ {Module, Class, Function}, edge indices
in range, no duplicate `(kind, name)`, no self-edges — then load it through the real
`GraphSerializer` (see the harness note under [Verification](#verification)).
**Output:** `graphs/babytrack.ai.json`, the reference graph for Part C.

### Dependency graph

```
B0 ─┬─ B1 ── B2 ── B3 ─────────────────┐
    │          └────── B6 ── B7 ── B8 ─┼── B10 ── B11
    ├─ B4 ─────────────────┘           │
    ├─ B5 ──────── B6                  │
    ├─ B9 ─────────────────────────────┘
    └─ B12 (anytime)                   B13 (needs Part A ≈A11)
```

B4, B5, B9, B12 are independent of the refactor and of each other. Serial spine is
B1→B2→B3 and B5→B6→B7. **≈40–50 h serial, ≈25–30 h with three parallel tracks.**

---

# Part C — AI-drawn vs tool-generated graphs

## Why this section exists

`repo_to_graph` is a heuristic regex scanner with no compiler and no type resolution. An AI
reading the same code has no such limits but is slower, non-deterministic, and can
hallucinate. The useful question isn't which is "better" — it's **which categories of graph
content each one gets right**, because that determines what the analyzer should be taught to
do and what should stay a curation step.

The answer feeds directly back into B6–B8 and into how merge mode gets used day to day: the
intended workflow is *AI draws the plan, the tool keeps it honest*, and that only works if
the tool's blind spots are known.

## The free calibration baseline

**C0 can run immediately — it does not wait on Part A or even on Part B.** This repo already
contains a matched pair covering the same codebase:

| | nodes | edges | authored by |
|---|---|---|---|
| `graphs/graph_draw.json` | 15 | 25 | a human, as an implementation plan |
| `graphs/graph_draw.generated.json` | 92 | 110 | `repo_to_graph` |

A 6× node-count gap is the headline finding before any tooling is written: the tool emits
every member function (76 of its 92 nodes are Functions), the human emitted six. That is an
*altitude* difference, not an accuracy one, and it is the single most important thing Part C
has to characterize — because a 300-node babytrack graph nobody reads is the main risk
flagged in the open items.

## Tasks

**C1 — comparison harness** · 3–4 h · needs B9 (or B10's `--dry-run`)
Reuse rather than rebuild: `GraphMerger` already computes `kept / added / stale` by
`(kind, label)`, which *is* the set comparison. Add a reporting mode that emits, treating the
AI graph as reference and the generated graph as candidate:
- **node agreement** per kind — matched, AI-only, tool-only
- **edge agreement** per label — matched, AI-only, tool-only, and edges present in both but
  with a *different* label (e.g. AI says "inherits", tool says "uses")
- **structural summary** — node counts by kind, mean out-degree, orphan count
Output machine-readable (JSON) plus a short human table. Lives with `graph_merge`;
`--dry-run` on the CLI is the entry point.
**Tests:** two hand-built scenes with known differences; assert every count.

**C2 — run C0's baseline and characterize the gap** · 2–3 h · needs C1
Run the harness over the existing `graph_draw.json` / `graph_draw.generated.json` pair.
Produce a written characterization, not just numbers, answering:
- Of the tool's 76 Function nodes, how many correspond to something a human would plan?
- Of the human's 15 nodes, how many does the tool find at all?
- Do the two agree on *module* structure even where they disagree on detail? (Expect yes —
  CMake targets are unambiguous, which suggests module-level extraction is solved and
  function-level is not.)
- Which of the human's 10 "uses" edges does the include heuristic actually recover?
This is the cheapest task in Part C and the most likely to change B6–B8's design, so **run it
before B7 if the schedule allows.**

**C3 — babytrack comparison** · 3–4 h · needs B13, C1
Same harness, on `graphs/babytrack.ai.json` (B13) versus `repo_to_graph`'s output for the
same workspace. The Rust-specific questions the C++ baseline cannot answer:
- Does `RustAnalyzer` find every crate and every intra-workspace dependency? *(Cargo.toml is
  declarative, so anything less than 100% here is a bug, not a heuristic limitation.)*
- Does it recover the `EventStore` trait relationship — `bt-store → bt-core` "inherits" — that
  A3/A2 deliberately planted?
- How much `use`-derived "uses" noise appears, given resolution is file-granular? Measure
  tool-only edges as a fraction of total.
- Do depth-1 nested modules land where the AI put conceptual boundaries, or does the
  filesystem disagree with the architecture?

**C4 — decide what the analyzer learns** · 2–3 h · needs C2, C3
Turn the findings into changes, sorted into three buckets:
- **Fix in the analyzer** — things it should have caught and didn't (expect: missed
  dependency edges, wrong module attribution).
- **Add a knob** — things where both answers are legitimate at different times. The
  `--no-functions` flag from the open items is the leading candidate; C2's altitude gap is
  the evidence for it.
- **Leave to curation** — things no regex scanner can produce: intent, rationale, and the
  *why* half of an implementation prompt. Document these explicitly in `graphs/README.md`
  so the tool's output is never mistaken for a finished plan.
**Deliverable:** a short written verdict plus whatever B-task follow-ups it generates.

**C5 — establish the round-trip workflow** · 2 h · needs C4, B10
Prove the intended loop end-to-end on babytrack: AI draws the plan with everything
`implemented: false` → build a slice → `repo_to_graph --merge` → confirm the built nodes flip
to `true`, the unbuilt ones stay `false` with their prompts intact, and
`GraphScene::readyToImplement()` names the genuinely next actionable work.
**Done when:** the merge is run twice in a row and the second is a no-op — the strongest
available evidence that merge mode is not quietly destroying curation.

---

## The planned graph

`graphs/graph_draw.planned.json` — **already written** — is this repo's architecture *as it
will stand once Part B is complete*, in the `graph_io` format, loadable in the GUI today.

| | |
|---|---|
| nodes | 67 — 10 Module, 20 Class, 37 Function |
| edges | 106 — 27 contains, 30 provides, 28 uses, 19 depends on, 2 inherits |
| status | 24 implemented (exists today), 43 not (Part B creates it) |

Verified by loading it through the real `GraphSerializer`, not just a schema check.
`readyToImplement()` returns 28 nodes and `blocked()` returns 15 — the ready set is the leaf
utilities (`sourcetext::blankC`, `rustlex::blank`, the `TomlDocument` methods, the
`LanguageAnalyzer` interface), and the aggregates stay blocked until their parts land. That
ordering matches the B-task dependency graph above, independently derived, which is a
reasonable sanity check on both.

Each unimplemented node's `comment` is a real implementation prompt carrying the specific
traps for that piece — the `r#type` raw-identifier trap on `rustlex::blank`, the
non-descending walk on `RepoFileIndex`, the length invariant on `sourcetext::blankC`. Open it
in the app to work through Part B, and once B9/B10 land, merge `repo_to_graph`'s output back
into it to watch the flags flip.

**Do not confuse the three graph files:** `graph_draw.json` is the curated present,
`graph_draw.generated.json` is the machine-scanned present, `graph_draw.planned.json` is the
intended future. Only the last one has unimplemented nodes.

---

## Sequencing

- **A0 gates the app side.** It's the only genuinely risky assumption in the plan.
- **B0 gates the graph_draw side** and takes 15 minutes.
- **C0/C2 gate nothing and can run today** — the calibration pair already exists, and the
  altitude finding should land before B7 fixes function-level extraction in place.
- Part B is independent of Part A until B13.
- Within Part A: A1 → A2/A3 (parallel) → A4 → A5, and A6 → A7 → A8–A11 (largely parallel).

---

## Verification

**graph_draw:**
```
cmake -S . -B build && cmake --build build
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure
```
Baseline confirmed: green today, 11 test cases / 96 assertions. New Catch2 cases follow the
existing `QTemporaryDir` + `nodesOfKind`/`nodeNamed`/`hasEdge` pattern
(`tests/test_repoanalyzer.cpp:12-81`), registered in `tests/CMakeLists.txt`.

**Loading a hand-written graph** (used to validate `graph_draw.planned.json`, and needed
again for B13): compile a throwaway `GraphSerializer::loadFromFile` harness against the built
static libs. On this Mac, Qt is a framework install and needs `-include arm_acle.h` to work
around a `qyieldcpu.h` warning-as-error:
```
clang++ -std=c++17 -include arm_acle.h loadcheck.cpp \
  -I graph_core -I graph_io -F /opt/homebrew/opt/qt/lib \
  -I /opt/homebrew/opt/qt/lib/Qt{Core,Gui,Widgets}.framework/Headers \
  build/graph_io/libgraph_io.a build/graph_core/libgraph_core.a \
  -framework QtWidgets -framework QtGui -framework QtCore -o loadcheck
QT_QPA_PLATFORM=offscreen ./loadcheck graphs/graph_draw.planned.json
```
Worth promoting to a real `graph_validate` target during B10.

**babytrack:** `cargo test --workspace`; `wasm-pack test --headless --chrome` for the
`bt-web` local store. Then the manual pass that actually matters (A15): both phones
installed, log a feed from each simultaneously, put one phone in airplane mode and log three
feeds, bring it back and confirm they land exactly once with no duplicates and no lost edits,
then reboot the desktop mid-sync and confirm recovery.

## Open items

- **Desktop OS** unconfirmed — affects only A14.
- **A0's outcome** determines whether A12 targets standalone launch or accepts a shortcut,
  and could redirect the frontend to a Tauri 2 shell. Nothing before A6 is wasted.
- **Rust graphs may be large** — a real axum+Leptos workspace could exceed 300 nodes. C2's
  altitude measurement on the existing 15-vs-92 pair is the evidence for whether
  `--no-functions` is needed, and it can be gathered before writing any Rust code.
