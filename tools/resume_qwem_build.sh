#!/usr/bin/env bash
# Resume the graph_draw traversal after an interruption.
#
#   ./tools/resume_qwem_build.sh                  # son-of-felnor (default)
#   ./tools/resume_qwem_build.sh felnor           # the original box
#   ./tools/resume_qwem_build.sh son-of-felnor
#
# --resume skips nodes already marked "done" in .harness/state.json instead of
# rebuilding them. Do NOT add --reset here: --reset deletes state.json before
# --resume ever gets to check it, silently discarding finished nodes.
#
# FREEZE LIST (reviewed 2026-08-28). All 9 remaining nodes — rustlex::blank,
# rustlex::leadingDoc, scanRustFile, TomlDocument::*, RustAnalyzer::* — live in
# graph_lang_rust, so every other module can be frozen. graph_lang came back
# onto this list today: it was dropped on 2026-08-25 because LabelResolver (B8)
# was still unbuilt inside it and freezing would have shadowed its own working
# directory, but LabelResolver is built now, so the hazard is gone.
# Re-check this list whenever the remaining set changes:
#   python3 -c "import json;d=json.load(open('graphs/graph_draw.planned.json'));\
#   print([x['name'] for x in d['nodes'] if not x.get('implemented')])"
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${1:-son-of-felnor}"
case "$HOST" in
  son-of-felnor)
    # llama.cpp, 65536 ctx per slot, 4 slots. Swapped 2026-08-29 to
    # Qwen3-Coder-30B-A3B-Instruct UD-Q8_K_XL (MoE, ~3B active params, NOT a
    # reasoning model) from the earlier Qwen3.8-27B, to fix the repeated
    # "explores for an hour, never writes anything" failures on rustlex::blank
    # and scanRustFile. son-of-felnor reports this model's id as its absolute
    # gguf path and opencode resolves --model against that exact key client
    # side (a clean alias 500s before the request even reaches llama.cpp) —
    # verified directly, not assumed. If sync-llama-models.py renames this
    # key on a future re-sync, update it here too.
    MODEL="son-of-felnor//home/joe/Downloads/Qwen3-Coder-30B-A3B-Instruct-UD-Q8_K_XL.gguf"
    CTX_CHARS=100000
    ;;
  felnor)
    # llama-swap, 32768 ctx, Qwen3.8-27B UD-Q6-K.
    MODEL="felnor/qwen3.8-27b-ud-q6-k"
    CTX_CHARS=55000
    ;;
  *)
    echo "unknown host: $HOST (expected 'felnor' or 'son-of-felnor')" >&2
    echo "or set MODEL= and CTX_CHARS= yourself and edit this script." >&2
    exit 2
    ;;
esac

echo "host   : $HOST"
echo "model  : $MODEL"
echo "context: $CTX_CHARS chars"

./graph_agent.py \
  --graph graphs/graph_draw.planned.json \
  --scope-config harness.scope.json \
  --plan-file IMPLEMENTATION_PLAN.md \
  --backend opencode \
  --model "$MODEL" \
  --agent build \
  --context-chars "$CTX_CHARS" \
  --max-tokens 8192 \
  --resume \
  --double-check 2 \
  --confirm-changes \
  --freeze 'graph_lang/**' \
  --freeze 'graph_lang_cpp/**' \
  --freeze 'graph_merge/**' \
  --freeze 'graph_analyze/**' \
  --context-file graph_lang/languageanalyzer.h \
  --context-file graph_lang/ir/ir.h \
  --context-file graph_lang/repofileindex.h \
  --context-file graph_lang/sourcetext.h \
  --context-file graph_lang_cpp/cppanalyzer.h \
  --stop-on-failure
