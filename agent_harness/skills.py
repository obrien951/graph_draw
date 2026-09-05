"""Agent skills: reusable instruction blocks the harness pastes into a brief.

A "skill" here is just a markdown file under ``agent_harness/skills/``. It is
backend-neutral on purpose — it goes into the brief text, so it works whether
the node is run by opencode, a llama-server completion, or a local gguf, none
of which share an agent-definition format.

``artifact_search`` is the one the harness injects automatically, for every
Artifact node (see graphmodel.KIND_ARTIFACT and prompts.PromptBuilder).
"""
from __future__ import annotations

import re
from functools import lru_cache
from pathlib import Path

_DIR = Path(__file__).with_name("skills")

ARTIFACT_SEARCH = "artifact_search"


@lru_cache(maxsize=32)
def _raw(name: str) -> str:
    return (_DIR / f"{name}.md").read_text(encoding="utf-8").strip()


def load(name: str, demote: int = 0) -> str:
    """The text of ``skills/<name>.md``. Raises FileNotFoundError if missing.

    ``demote`` pushes every markdown heading down that many levels so the
    skill's own ``# Title`` does not collide with the brief's headings when it
    is pasted in as a section.
    """
    text = _raw(name)
    if demote > 0:
        text = re.sub(r"^(#{1,5}) ", lambda m: "#" * min(6, len(m.group(1)) + demote) + " ",
                      text, flags=re.M)
    return text


def available() -> list[str]:
    return sorted(p.stem for p in _DIR.glob("*.md"))
