"""External reference resources — the "web search, not AI reasoning" channel.

A recurring failure mode when a small local model implements a node whose
specification names a *published* artefact — the VADER sentiment lexicon, the
Loughran-McDonald financial word lists, an ISO currency table, a stopword
list, an RFC's grammar — is that the model reconstructs it from its own
training memory. The result looks plausible and is quietly wrong: invented
valence scores, a half-remembered category list, dropped entries.

The harness cannot force a sandboxed agent onto the network, but it can:

* state plainly in the brief that reconstructing a named public dataset from
  memory is a defect, not a shortcut (see prompts.REFERENCE_DATA), and
* carry an explicit, per-graph list of those resources — name, URL, and where
  the real file should live — so the agent (or `--fetch-refs`) can pull the
  genuine article and load it instead of guessing.

The list lives in the scope config under ``reference_resources`` (top level,
and/or per-module). Each entry:

    {
      "name": "VADER lexicon",
      "url": "https://raw.githubusercontent.com/cjhutto/vaderSentiment/master/vaderSentiment/vader_lexicon.txt",
      "path": "src/sentiment/data/vader_lexicon.txt",   # where the code loads it from
      "note": "tab-separated: token<TAB>mean<TAB>std<TAB>[raw ratings]",
      "modules": ["sentiment"]                            # optional; omit = all nodes
    }
"""
from __future__ import annotations

import hashlib
import urllib.error
import urllib.request
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Iterable, Sequence

_TIMEOUT = 30
_MAX_BYTES = 64 * 1024 * 1024


@dataclass(frozen=True)
class ReferenceResource:
    name: str
    url: str = ""
    path: str = ""          # repo-relative location the node's code loads from
    note: str = ""
    modules: tuple[str, ...] = ()
    local_path: str = ""    # filled by fetch(): where a real copy actually sits

    @classmethod
    def parse(cls, raw: dict) -> "ReferenceResource":
        mods = raw.get("modules") or raw.get("module") or ()
        if isinstance(mods, str):
            mods = (mods,)
        return cls(
            name=str(raw.get("name") or raw.get("url") or "reference"),
            url=str(raw.get("url", "")),
            path=str(raw.get("path", "")),
            note=str(raw.get("note", "")),
            modules=tuple(str(m) for m in mods),
        )

    def applies_to(self, module_name: str | None) -> bool:
        return not self.modules or (module_name or "") in self.modules

    def describe(self) -> str:
        bits = [f"- {self.name}"]
        if self.url:
            bits.append(f"    source: {self.url}")
        if self.path:
            bits.append(f"    the code loads it from: {self.path}")
        if self.local_path and self.local_path != self.path:
            bits.append(f"    a real copy is already on disk at: {self.local_path}")
        if self.note:
            bits.append(f"    format: {self.note}")
        return "\n".join(bits)


def parse_all(raw_list: Iterable[dict] | None) -> tuple[ReferenceResource, ...]:
    return tuple(ReferenceResource.parse(r) for r in (raw_list or []) if isinstance(r, dict))


def for_module(resources: Sequence[ReferenceResource], module_name: str | None) -> list[ReferenceResource]:
    return [r for r in resources if r.applies_to(module_name)]


def _cache_name(res: ReferenceResource) -> str:
    stem = Path(res.path).name or hashlib.sha1(res.url.encode()).hexdigest()[:16]
    return stem or "reference.dat"


def fetch(resources: Sequence[ReferenceResource], root: str | Path,
          cache_dir: str | Path, log=print) -> list[ReferenceResource]:
    """Download every resource that has a URL and isn't already on disk.

    Fails open: a resource that cannot be fetched is returned unchanged (no
    ``local_path``), the brief still names its URL, and the run continues.
    A resource whose ``path`` already exists in the repo is left alone — the
    real file is already there.
    """
    root = Path(root)
    cache_dir = Path(cache_dir)
    out: list[ReferenceResource] = []
    for res in resources:
        in_repo = (root / res.path) if res.path else None
        if in_repo and in_repo.is_file() and in_repo.stat().st_size > 0:
            out.append(replace(res, local_path=res.path))
            continue
        if not res.url:
            out.append(res)
            continue
        dest = cache_dir / _cache_name(res)
        if not (dest.is_file() and dest.stat().st_size > 0):
            try:
                cache_dir.mkdir(parents=True, exist_ok=True)
                _download(res.url, dest)
                log(f"refs: fetched {res.name} -> {dest}")
            except (urllib.error.URLError, OSError, ValueError) as exc:
                log(f"refs: could not fetch {res.name} ({exc}); the brief will "
                    f"point the agent at {res.url}")
                out.append(res)
                continue
        try:
            rel = str(dest.relative_to(root))
        except ValueError:
            rel = str(dest)
        out.append(replace(res, local_path=rel))
    return out


def _download(url: str, dest: Path) -> None:
    if not url.lower().startswith(("http://", "https://")):
        raise ValueError(f"refusing non-http url: {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "graph-agent-harness/1.0"})
    with urllib.request.urlopen(req, timeout=_TIMEOUT) as resp:  # noqa: S310 (http(s) enforced above)
        data = resp.read(_MAX_BYTES + 1)
    if len(data) > _MAX_BYTES:
        raise ValueError(f"{url} exceeds {_MAX_BYTES} byte cap")
    tmp = dest.with_suffix(dest.suffix + ".part")
    tmp.write_bytes(data)
    tmp.replace(dest)
