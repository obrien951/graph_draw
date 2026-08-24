#!/usr/bin/env bash
# B3 acceptance harness: prove the refactored analyzer still produces the same
# graph as the original.
#
# The committed baseline graphs/graph_draw.generated.json was scanned from this
# repo BEFORE the Part B refactor added graph_lang/ etc. Scanning the working
# tree therefore legitimately finds MORE nodes, and a direct comparison fails for
# reasons that have nothing to do with analyzer behaviour.
#
# So we hold the INPUT fixed: check out the pre-refactor commit in a worktree and
# scan THAT with the newly built binary. Any difference is then a real behaviour
# change.
#
# Compared as sorted multisets, not a text diff: QDirIterator ordering is not
# guaranteed stable, and node positions are layout, not meaning.
set -euo pipefail

BASE_COMMIT="${1:-7330653}"
WORKTREE=/tmp/gd_baseline
BIN=./build/repo_to_graph

[ -x "$BIN" ] || { echo "error: $BIN not built. Run cmake --build build first."; exit 2; }
git worktree add -q "$WORKTREE" "$BASE_COMMIT" 2>/dev/null || true
"$BIN" "$WORKTREE" /tmp/parity_scan.json >/dev/null

python3 - <<'PY'
import json, sys
def sig(p):
    d = json.load(open(p)); n = d["nodes"]
    return (sorted((x["kind"], x["name"]) for x in n),
            sorted((n[e["origin"]]["name"], n[e["destination"]]["name"], e["comment"])
                   for e in d["edges"]))
want, got = sig("graphs/graph_draw.generated.json"), sig("/tmp/parity_scan.json")
ok = True
for what, w, g in (("nodes", want[0], got[0]), ("edges", want[1], got[1])):
    if w == g:
        print(f"  {what}: identical ({len(w)})")
    else:
        ok = False
        miss, extra = set(w) - set(g), set(g) - set(w)
        print(f"  {what}: DIFFER  missing={len(miss)} unexpected={len(extra)}")
        for x in sorted(miss)[:10]:  print("    missing:   ", x)
        for x in sorted(extra)[:10]: print("    unexpected:", x)
print("PARITY OK" if ok else "PARITY FAILED")
sys.exit(0 if ok else 1)
PY
