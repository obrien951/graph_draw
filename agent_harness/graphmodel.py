"""The graph_io JSON graph, loaded as an implementation plan.

Convention (graphs/README.md): an edge ``A -> B`` means *A depends on B*, so a
node's dependencies are the targets of its outgoing edges.  Everything the
harness needs to decide "what do I build, in what order, and who owns this
node" lives here and nowhere else.
"""
from __future__ import annotations

import heapq
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Iterator, Sequence

KIND_MODULE = "Module"
KIND_CLASS = "Class"
KIND_FUNCTION = "Function"
KIND_ORDER = {KIND_MODULE: 0, KIND_CLASS: 1, KIND_FUNCTION: 2}

#: Edge labels that mean "the origin owns the destination".
OWNERSHIP_LABELS = ("contains", "provides")

ROOT_FIRST = "root-first"
LEAF_FIRST = "leaf-first"
ORDERS = (ROOT_FIRST, LEAF_FIRST)


@dataclass
class Node:
    index: int
    kind: str
    name: str
    comment: str = ""
    implemented: bool = False
    raw: dict = field(default_factory=dict, repr=False)

    @property
    def slug(self) -> str:
        return re.sub(r"[^A-Za-z0-9_.-]+", "_", self.name).strip("_") or f"node{self.index}"

    def to_json(self) -> dict:
        out = dict(self.raw)
        out["kind"] = self.kind
        out["name"] = self.name
        out["comment"] = self.comment
        out["implemented"] = self.implemented
        return out


@dataclass(frozen=True)
class Edge:
    origin: int
    destination: int
    comment: str = ""

    def to_json(self) -> dict:
        return {"origin": self.origin, "destination": self.destination, "comment": self.comment}


class GraphError(RuntimeError):
    pass


class Graph:
    """Nodes plus directed dependency edges, with the queries the runner needs."""

    def __init__(self, nodes: Sequence[Node], edges: Sequence[Edge], path: Path | None = None):
        self.nodes = list(nodes)
        self.edges = [e for e in edges if self._valid(e)]
        self.path = path
        self._out: dict[int, list[Edge]] = {i: [] for i in range(len(self.nodes))}
        self._in: dict[int, list[Edge]] = {i: [] for i in range(len(self.nodes))}
        for e in self.edges:
            self._out[e.origin].append(e)
            self._in[e.destination].append(e)
        self._by_name: dict[str, int] = {}
        for n in self.nodes:
            self._by_name.setdefault(n.name, n.index)

    # ---------------------------------------------------------------- loading
    def _valid(self, e: Edge) -> bool:
        last = len(self.nodes) - 1
        return 0 <= e.origin <= last and 0 <= e.destination <= last and e.origin != e.destination

    @classmethod
    def load(cls, path: str | Path) -> "Graph":
        path = Path(path)
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise GraphError(f"cannot read graph {path}: {exc}") from exc
        if not isinstance(data, dict) or "nodes" not in data:
            raise GraphError(f"{path} is not a graph_io graph (no 'nodes' key)")
        nodes = []
        for i, raw in enumerate(data.get("nodes") or []):
            nodes.append(Node(
                index=i,
                kind=str(raw.get("kind", KIND_CLASS)),
                name=str(raw.get("name", f"node{i}")),
                comment=str(raw.get("comment", "")),
                implemented=bool(raw.get("implemented", False)),
                raw=dict(raw),
            ))
        edges = [
            Edge(int(r.get("origin", -1)), int(r.get("destination", -1)), str(r.get("comment", "")))
            for r in (data.get("edges") or [])
        ]
        return cls(nodes, edges, path)

    def to_json(self) -> dict:
        return {"nodes": [n.to_json() for n in self.nodes], "edges": [e.to_json() for e in self.edges]}

    def save(self, path: str | Path | None = None) -> Path:
        target = Path(path or self.path or "")
        if not str(target):
            raise GraphError("no path to save the graph to")
        target.write_text(json.dumps(self.to_json(), indent=4) + "\n", encoding="utf-8")
        return target

    # ---------------------------------------------------------------- queries
    def __len__(self) -> int:
        return len(self.nodes)

    def __iter__(self) -> Iterator[Node]:
        return iter(self.nodes)

    def node(self, index: int) -> Node:
        return self.nodes[index]

    def find(self, name: str) -> Node | None:
        i = self._by_name.get(name)
        if i is None:
            lowered = name.lower()
            for n in self.nodes:
                if n.name.lower() == lowered:
                    return n
            return None
        return self.nodes[i]

    def dependencies(self, index: int) -> list[Node]:
        return [self.nodes[e.destination] for e in self._out[index]]

    def dependents(self, index: int) -> list[Node]:
        return [self.nodes[e.origin] for e in self._in[index]]

    def outgoing(self, index: int) -> list[Edge]:
        return list(self._out[index])

    def incoming(self, index: int) -> list[Edge]:
        return list(self._in[index])

    def unimplemented_dependencies(self, index: int) -> list[Node]:
        return [n for n in self.dependencies(index) if not n.implemented]

    def owner_module(self, index: int) -> Node | None:
        """The Module node that (transitively) contains this node, if any."""
        seen = {index}
        frontier = [index]
        while frontier:
            nxt = []
            for i in frontier:
                if i != index and self.nodes[i].kind == KIND_MODULE:
                    return self.nodes[i]
                for e in self._in[i]:
                    owner_edge = any(lbl in e.comment.lower() for lbl in OWNERSHIP_LABELS)
                    if owner_edge and e.origin not in seen:
                        seen.add(e.origin)
                        nxt.append(e.origin)
            frontier = nxt
        return None

    def owner(self, index: int) -> Node | None:
        """The single node that directly owns this one (one contains/provides
        hop up — a Function's Class, or a Class'/Function's Module), if any.

        Unlike owner_module, this does not walk past the first owner: a
        Function almost never carries dependency edges of its own (they sit
        on the Class that provides it), so a caller wanting "what does this
        node actually depend on" needs the immediate owner specifically, not
        wherever the module chain eventually bottoms out.
        """
        for e in self._in[index]:
            if any(lbl in e.comment.lower() for lbl in OWNERSHIP_LABELS):
                return self.nodes[e.origin]
        return None

    def order(self, strategy: str = ROOT_FIRST) -> tuple[list[int], list[int]]:
        """Return (ordered node indices, indices involved in a cycle).

        ``root-first`` — the "foot to leaf" walk: a node is visited before the
        dependencies it will call, so each agent stubs what does not exist yet.
        ``leaf-first`` — the opposite: dependencies land before their callers.
        """
        if strategy not in ORDERS:
            raise GraphError(f"unknown order {strategy!r}; expected one of {ORDERS}")
        # precedence[a] -> b means a must be emitted before b
        successors: dict[int, set[int]] = {i: set() for i in range(len(self.nodes))}
        indeg = {i: 0 for i in range(len(self.nodes))}
        for e in self.edges:
            a, b = (e.origin, e.destination) if strategy == ROOT_FIRST else (e.destination, e.origin)
            if b in successors[a]:
                continue
            successors[a].add(b)
            indeg[b] += 1
        heap = [self._sort_key(i) for i in range(len(self.nodes)) if indeg[i] == 0]
        heapq.heapify(heap)
        order: list[int] = []
        while heap:
            _, _, i = heapq.heappop(heap)
            order.append(i)
            for j in sorted(successors[i]):
                indeg[j] -= 1
                if indeg[j] == 0:
                    heapq.heappush(heap, self._sort_key(j))
        cyclic = [i for i in range(len(self.nodes)) if i not in set(order)]
        order.extend(sorted(cyclic, key=lambda i: self._sort_key(i)))
        return order, cyclic

    def _sort_key(self, index: int) -> tuple[int, int, int]:
        node = self.nodes[index]
        return (KIND_ORDER.get(node.kind, 9), index, index)

    def pending(self, order: Iterable[int]) -> list[int]:
        return [i for i in order if not self.nodes[i].implemented]

    def mark_implemented(self, index: int, value: bool = True) -> None:
        self.nodes[index].implemented = value

    def summary(self) -> str:
        kinds: dict[str, int] = {}
        for n in self.nodes:
            kinds[n.kind] = kinds.get(n.kind, 0) + 1
        done = sum(1 for n in self.nodes if n.implemented)
        parts = ", ".join(f"{v} {k}" for k, v in sorted(kinds.items()))
        return f"{len(self.nodes)} nodes ({parts}), {len(self.edges)} edges, {done} implemented, {len(self.nodes) - done} to build"
