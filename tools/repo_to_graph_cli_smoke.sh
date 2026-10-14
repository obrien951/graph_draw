#!/usr/bin/env bash
# R5 acceptance harness: exercise repo_to_graph's --lang/--merge/--dry-run flags
# end to end against a synthetic Rust fixture, the same way tools/analyzer_parity.sh
# exercises the plain scan path. Not wired into ctest (it shells out to the built
# binary and writes real files, like analyzer_parity.sh) — run it by hand after
# touching analyze/main.cpp, graph_merge, or RepoAnalyzer's CLI-facing surface.
set -euo pipefail

BIN=./build/repo_to_graph
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[ -x "$BIN" ] || { echo "error: $BIN not built. Run cmake --build build first."; exit 2; }
export QT_QPA_PLATFORM=offscreen

REPO="$WORK/demo"
mkdir -p "$REPO/src"
cat > "$REPO/Cargo.toml" <<'EOF'
[package]
name = "demo"
version = "0.1.0"
EOF
cat > "$REPO/src/main.rs" <<'EOF'
/// A user account.
#[derive(Debug)]
struct User { name: String }

impl User {
    fn greet(&self) { println!("hi {}", self.name); }
}

fn main() {
    User { name: "a".into() }.greet();
}
EOF

fail=0

echo "[1] fresh scan, no --merge"
"$BIN" --lang rust "$REPO" "$WORK/out1.json" >/dev/null
nodes=$(python3 -c "import json;print(len(json.load(open('$WORK/out1.json'))['nodes']))")
[ "$nodes" = "4" ] && echo "  ok: 4 nodes" || { echo "  FAIL: expected 4 nodes, got $nodes"; fail=1; }

echo "[2] --merge round-trip preserves curated comment/position, flips implemented"
cat > "$WORK/curated.json" <<'EOF'
{"nodes":[{"comment":"Planned: user account type.","implemented":false,"kind":"Class","name":"User","position":{"x":500,"y":500}}],"edges":[]}
EOF
"$BIN" --lang rust --merge "$WORK/curated.json" "$REPO" "$WORK/merged.json" | grep -q "^kept 1 . added 3 . stale 0$" \
    && printf '  ok: kept 1 \xc2\xb7 added 3 \xc2\xb7 stale 0\n' || { echo "  FAIL: unexpected merge stats"; fail=1; }
python3 - "$WORK/merged.json" <<'PY' || fail=1
import json, sys
d = json.load(open(sys.argv[1]))
u = next(n for n in d["nodes"] if n["name"] == "User")
assert u["implemented"] is True, u
assert u["comment"] == "Planned: user account type.", u
assert u["position"] == {"x": 500, "y": 500}, u
print("  ok: User now implemented, curated comment/position kept")
PY

echo "[3] --dry-run without --merge is rejected"
if "$BIN" --dry-run --lang rust "$REPO" "$WORK/should_not_exist.json" 2>/dev/null; then
    echo "  FAIL: should have been rejected"; fail=1
else
    [ -e "$WORK/should_not_exist.json" ] && { echo "  FAIL: wrote a file anyway"; fail=1; } \
        || echo "  ok: rejected, nothing written"
fi

echo "[4] --merge never redirects the output path (anti-clobber)"
before=$(md5 -q "$WORK/out1.json" 2>/dev/null || md5sum "$WORK/out1.json" | cut -d' ' -f1)
"$BIN" --lang rust --merge "$WORK/out1.json" "$REPO" >/dev/null
after=$(md5 -q "$WORK/out1.json" 2>/dev/null || md5sum "$WORK/out1.json" | cut -d' ' -f1)
[ "$before" = "$after" ] && [ -f "$REPO/graphs/demo.generated.json" ] \
    && echo "  ok: merge input untouched; wrote to <repo>/graphs/demo.generated.json instead" \
    || { echo "  FAIL: merge input was modified or output path was wrong"; fail=1; }

if [ "$fail" = "0" ]; then
    echo "SMOKE OK"
else
    echo "SMOKE FAILED"
    exit 1
fi
