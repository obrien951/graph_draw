"""Run state, so a long walk can be stopped and resumed."""
from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

PENDING, RUNNING, DONE, FAILED, SKIPPED = "pending", "running", "done", "failed", "skipped"


@dataclass
class NodeState:
    name: str
    kind: str = ""
    status: str = PENDING
    attempts: int = 0
    changed: list[str] = field(default_factory=list)
    violations: list[str] = field(default_factory=list)
    note: str = ""
    seconds: float = 0.0
    finished_at: str = ""
    verified: bool = False   # the build/test gate actually ran and passed
    reviewed: bool = False   # a review agent actually read the diff


class RunState:
    """A JSON file under the state dir holding one record per node."""

    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.started_at = time.strftime("%Y-%m-%d %H:%M:%S")
        self.nodes: dict[str, NodeState] = {}
        self.meta: dict = {}

    @classmethod
    def load(cls, path: str | Path) -> "RunState":
        state = cls(path)
        if state.path.exists():
            try:
                data = json.loads(state.path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                return state
            state.started_at = data.get("started_at", state.started_at)
            state.meta = data.get("meta", {})
            for name, raw in (data.get("nodes") or {}).items():
                state.nodes[name] = NodeState(**{**NodeState(name=name).__dict__, **raw})
        return state

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "started_at": self.started_at,
            "meta": self.meta,
            "nodes": {name: asdict(ns) for name, ns in self.nodes.items()},
        }
        self.path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    def get(self, name: str, kind: str = "") -> NodeState:
        if name not in self.nodes:
            self.nodes[name] = NodeState(name=name, kind=kind)
        return self.nodes[name]

    def status_of(self, name: str) -> str:
        return self.nodes[name].status if name in self.nodes else PENDING

    def counts(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for ns in self.nodes.values():
            out[ns.status] = out.get(ns.status, 0) + 1
        return out
