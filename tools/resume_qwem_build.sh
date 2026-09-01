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
    # llama.cpp, now a preset router (like felnor) instead of a single-file
    # server, so son-of-felnor's /v1/models reports clean alias ids
    # ("qwen3-coder-30b-q8xl") rather than the old raw gguf path — the raw
    # path this used to be pinned to (Qwen3-Coder-30B-A3B-Instruct-UD-Q8_K_XL
    # .gguf as the literal --model string) no longer matches any key in
    # opencode.json's son-of-felnor.models map and would 500 client-side.
    # Confirmed against a live `curl son-of-felnor:8080/v1/models` and the
    # current opencode.json, 2026-09-01.
    #
    # 2026-09-01: two more things changed after 11 rounds of code quality
    # not matching Qwen3-Coder's published benchmarks:
    #  - REVIEW_MODEL now points at qwen3-27b-q8-nothink instead of reusing
    #    the coder model for review. This is the split from 2026-08-30
    #    (regular Qwen3 reviews, Qwen3-Coder implements/fixes) actually
    #    getting wired up here for the first time — this script never
    #    passed --review-model before, so review had silently been running
    #    on the coder model this whole time. The coder model itself has no
    #    thinking mode to disable (Qwen3-Coder-30B-A3B-Instruct is
    #    instruct-only); qwen3-27b-q8 does, and opencode.json declared it
    #    non-reasoning while the server preset behind it left thinking on —
    #    -nothink is the preset that actually sets
    #    --chat-template-kwargs enable_thinking:false.
    #  - CTX_CHARS raised 100000 -> 150000: opencode.json's declared context
    #    ceiling for every son-of-felnor model was 32768 tokens (~130K
    #    chars) until today, well under Qwen3-Coder-30B-A3B-Instruct's real
    #    262144-token native context (confirmed on its HF model card) —
    #    100000 chars of assembled context plus opencode's own system
    #    prompt/tool schemas was likely sitting at or over that ceiling and
    #    getting client-side truncated before the model ever saw it. Now
    #    that the declared ceiling is raised to match the model's real
    #    context (your edit, not mine), there's headroom to raise this too.
    #    150000 is a deliberately moderate step, not the new ceiling itself —
    #    every char here costs prefill time on two GPUs; raise further only
    #    if you check it isn't costing you round-trip time you'd rather
    #    spend on --fix-rounds.
    MODEL="son-of-felnor/qwen3-coder-30b-q8xl"
    REVIEW_MODEL="son-of-felnor/qwen3-27b-q8-nothink"
    CTX_CHARS=150000
    ;;
  felnor)
    # llama-swap, 32768 ctx, Qwen3.8-27B UD-Q6-K.
    MODEL="felnor/qwen3.8-27b-ud-q6-k"
    REVIEW_MODEL=""
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
echo "review : ${REVIEW_MODEL:-(same as model)}"
echo "context: $CTX_CHARS chars"

REVIEW_ARGS=()
if [[ -n "$REVIEW_MODEL" ]]; then
  REVIEW_ARGS=(--review-model "$REVIEW_MODEL")
fi

# Per-node symbol index (agent_harness/indexer.py, added 2026-09-01): every
# prompt's "Existing symbols" section is now backed by `ctags` (universal-ctags
# specifically — the Xcode-bundled BSD ctags on PATH by default on macOS
# doesn't understand -R/--output-format=json and is rejected on sight) when
# it's installed, with a regex scan as the automatic fallback when it isn't.
# No flag here controls it — it runs by default, freshly, before every node's
# prompt is built, same as the symbol section already did. `find_ctags()`
# returns None on this machine right now (only BSD ctags on PATH), so this is
# currently still running the regex fallback; `brew install universal-ctags`
# to get the line-numbered, member-aware version (that formula conflicts with
# the pre-installed `ctags` name, so `brew link --overwrite universal-ctags`
# or use its `ctags-universal`-named binary — your call, not run here).

./graph_agent.py \
  --graph graphs/graph_draw.planned.json \
  --scope-config harness.scope.json \
  --plan-file IMPLEMENTATION_PLAN.md \
  --backend opencode \
  --model "$MODEL" \
  "${REVIEW_ARGS[@]+"${REVIEW_ARGS[@]}"}" \
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
