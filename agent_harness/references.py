"""External reference resources — the "web search, not AI reasoning" channel.

A recurring failure mode when a small local model implements a node whose
specification names a *published* artefact — the VADER sentiment lexicon, the
Loughran-McDonald financial word lists, an ISO currency table, a stopword
list, an RFC's grammar — is that the model reconstructs it from its own
training memory. The result looks plausible and is quietly wrong: invented
valence scores, a half-remembered category list, dropped entries.

The fix is to go and get the real file. This module retrieves an artefact the
way a person would: **run a search-engine query, look at the results, pick the
primary source, download from there.** A hard-coded ``url`` is still accepted
(as a hint / fallback), but the intended path is ``search``:

    {
      "name": "VADER lexicon",
      "search": "VADER sentiment lexicon vader_lexicon.txt cjhutto vaderSentiment raw",
      "path": "src/sentiment/data/vader_lexicon.txt",   # where the code loads it from
      "note": "tab-separated: token<TAB>mean<TAB>std<TAB>[raw ratings]",
      "modules": ["sentiment"]                            # optional; omit = all nodes
    }

An Artifact node (graphmodel.KIND_ARTIFACT) carries the same fields directly on
the node. ``reference_resources`` entries live in the scope config (top level,
and/or inside a module entry).

The search engine is DuckDuckGo's keyless "lite" endpoint by default; override
it with ``HARNESS_SEARCH_URL`` (env) or ``"search_url"`` in the scope config —
either a form endpoint that takes ``q=`` (DuckDuckGo html/lite) or a URL
template containing ``{query}`` (a SearXNG instance's ``/search?...&format=json``
works too).
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, replace
from datetime import datetime, timezone
from html.parser import HTMLParser
from pathlib import Path
from typing import Iterable, Sequence

_TIMEOUT = 30
_MAX_BYTES = 64 * 1024 * 1024

#: Keyless, returns plain HTML, tolerant of a scripted client.
DEFAULT_SEARCH_URL = "https://lite.duckduckgo.com/lite/"
_BROWSERISH_UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                  "(KHTML, like Gecko) Chrome/125.0 Safari/537.36")


@dataclass(frozen=True)
class ReferenceResource:
    name: str
    url: str = ""           # direct source, if one is pinned (a hint / fallback)
    search: str = ""        # search-engine query used to DISCOVER the source
    path: str = ""          # repo-relative location the node's code loads from
    note: str = ""
    modules: tuple[str, ...] = ()
    local_path: str = ""    # filled by fetch(): where a real copy actually sits
    source_url: str = ""    # filled by fetch(): the URL a copy was pulled from
    candidates: tuple[str, ...] = ()   # filled by fetch(): ranked search results
    sha256: str = ""        # expected hex digest, if the graph pins one
    license: str = ""       # SPDX id or short name, if known

    @classmethod
    def parse(cls, raw: dict) -> "ReferenceResource":
        mods = raw.get("modules") or raw.get("module") or ()
        if isinstance(mods, str):
            mods = (mods,)
        return cls(
            name=str(raw.get("name") or raw.get("url") or raw.get("search") or "reference"),
            url=str(raw.get("url", "")),
            search=str(raw.get("search", "")),
            path=str(raw.get("path", "")),
            note=str(raw.get("note", "")),
            modules=tuple(str(m) for m in mods),
            sha256=str(raw.get("sha256", "")).lower().strip(),
            license=str(raw.get("license", "")),
        )

    @classmethod
    def from_node(cls, node) -> "ReferenceResource":
        """An Artifact node's own fields as a resource. The node's `comment`
        is the human description; `search`/`url`/`path`/`sha256`/`license`/
        `format` (or `note`) come straight off the graph JSON."""
        raw = dict(getattr(node, "raw", {}) or {})
        raw.setdefault("name", getattr(node, "name", "artifact"))
        raw.setdefault("note", raw.get("format", ""))
        return cls.parse(raw)

    def query(self) -> str:
        """The search query to use: the explicit one, else the name."""
        return self.search or self.name

    def applies_to(self, module_name: str | None) -> bool:
        return not self.modules or (module_name or "") in self.modules

    def describe(self) -> str:
        bits = [f"- {self.name}"]
        if self.search:
            bits.append(f"    find it by searching for: {self.search}")
        if self.url:
            bits.append(f"    pinned source hint: {self.url}")
        if self.source_url:
            bits.append(f"    a copy was pulled from: {self.source_url}")
        if self.path:
            bits.append(f"    the code loads it from: {self.path}")
        if self.local_path and self.local_path != self.path:
            bits.append(f"    a real copy is already on disk at: {self.local_path}")
        if self.sha256:
            bits.append(f"    sha256: {self.sha256}")
        if self.license:
            bits.append(f"    license: {self.license}")
        if self.note:
            bits.append(f"    format: {self.note}")
        return "\n".join(bits)


def parse_all(raw_list: Iterable[dict] | None) -> tuple[ReferenceResource, ...]:
    return tuple(ReferenceResource.parse(r) for r in (raw_list or []) if isinstance(r, dict))


def for_module(resources: Sequence[ReferenceResource], module_name: str | None) -> list[ReferenceResource]:
    return [r for r in resources if r.applies_to(module_name)]


# ----------------------------------------------------------------- web search

@dataclass(frozen=True)
class SearchHit:
    url: str
    title: str = ""
    snippet: str = ""


#: Hosts that are search infrastructure, not a result worth downloading from.
_SEARCH_HOSTS = ("duckduckgo.com", "google.com", "bing.com", "search.marginalia.nu",
                 "startpage.com", "ecosia.org", "yahoo.com")


class _AnchorParser(HTMLParser):
    """Collects (href, visible-text) for every <a> in a results page."""

    def __init__(self) -> None:
        super().__init__()
        self.hits: list[tuple[str, str]] = []
        self._href: str | None = None
        self._text: list[str] = []

    def handle_starttag(self, tag, attrs):
        if tag == "a":
            href = dict(attrs).get("href")
            if href:
                self._flush()
                self._href = href

    def handle_data(self, data):
        if self._href is not None:
            self._text.append(data)

    def handle_endtag(self, tag):
        if tag == "a":
            self._flush()

    def _flush(self):
        if self._href is not None:
            self.hits.append((self._href, " ".join(self._text).strip()))
        self._href, self._text = None, []

    def close(self):
        super().close()
        self._flush()


def _unwrap(href: str) -> str | None:
    """A real destination URL from a results-page anchor, or None to drop it."""
    if href.startswith("//"):
        href = "https:" + href
    try:
        parts = urllib.parse.urlsplit(href)
    except ValueError:
        return None
    host = parts.hostname or ""
    # DuckDuckGo wraps every result as /l/?uddg=<encoded real url>
    if host.endswith("duckduckgo.com") and parts.path.startswith("/l/"):
        q = urllib.parse.parse_qs(parts.query)
        target = (q.get("uddg") or [""])[0]
        return target or None
    if parts.scheme not in ("http", "https"):
        return None
    if any(host == h or host.endswith("." + h) for h in _SEARCH_HOSTS):
        return None
    return href


def _parse_searxng_json(text: str) -> list[SearchHit]:
    try:
        data = json.loads(text)
    except (ValueError, TypeError):
        return []
    rows = data.get("results") if isinstance(data, dict) else None
    if not isinstance(rows, list):
        return []
    out = []
    for r in rows:
        if isinstance(r, dict) and r.get("url"):
            out.append(SearchHit(str(r["url"]), str(r.get("title", "")), str(r.get("content", ""))))
    return out


def _parse_html_hits(text: str) -> list[SearchHit]:
    parser = _AnchorParser()
    try:
        parser.feed(text)
        parser.close()
    except Exception:  # noqa: BLE001 — a malformed page must not crash a run
        pass
    seen: set[str] = set()
    out: list[SearchHit] = []
    for href, label in parser.hits:
        target = _unwrap(href)
        if not target or target in seen:
            continue
        seen.add(target)
        out.append(SearchHit(target, label))
    return out


def web_search(query: str, *, engine: str | None = None, limit: int = 8,
               timeout: int = _TIMEOUT, log=print) -> list[SearchHit]:
    """Run *query* against a search engine and return ranked result URLs.

    Default engine: DuckDuckGo's keyless lite endpoint. Override with the
    ``engine`` argument, ``HARNESS_SEARCH_URL`` in the environment, or
    ``"search_url"`` in the scope config. An engine string containing
    ``{query}`` is fetched with GET (and parsed as SearXNG JSON if it looks
    like JSON); otherwise it is POSTed ``q=<query>`` as a form.

    Fails open: any network or parse error logs and returns ``[]``.
    """
    engine = engine or os.environ.get("HARNESS_SEARCH_URL") or DEFAULT_SEARCH_URL
    try:
        if "{query}" in engine:
            body = _http(engine.replace("{query}", urllib.parse.quote(query)), timeout=timeout)
        else:
            body = _http(engine, data=urllib.parse.urlencode({"q": query}).encode(),
                         timeout=timeout)
    except (urllib.error.URLError, OSError, ValueError) as exc:
        log(f"refs: search for {query!r} failed ({exc})")
        return []
    text = body.decode("utf-8", "replace")
    hits = _parse_searxng_json(text) or _parse_html_hits(text)
    return hits[:limit]


# --------------------------------------------------------------- retrieval

_RAWISH_HOSTS = ("raw.githubusercontent.com", "raw.github.com", "gist.githubusercontent.com",
                 "gitlab.com", "bitbucket.org", "zenodo.org", "ndownloader.figshare.com",
                 "objects.githubusercontent.com", "media.githubusercontent.com")


def _to_raw_url(url: str) -> str:
    """Rewrite a code-host *view* URL to its raw-content equivalent, so a
    search result that points at a repo's file page still downloads the file.
    Anything not recognised is returned unchanged."""
    p = urllib.parse.urlsplit(url)
    host, seg = (p.hostname or ""), p.path.split("/")
    if host == "github.com" and "blob" in seg:
        i = seg.index("blob")
        return urllib.parse.urlunsplit((
            "https", "raw.githubusercontent.com",
            "/".join(seg[1:i] + seg[i + 1:]), "", ""))
    if host in ("gitlab.com", "bitbucket.org") and "-" in seg and "blob" in seg:
        return url.replace("/-/blob/", "/-/raw/", 1)
    if host == "bitbucket.org" and "src" in seg:
        return url.replace("/src/", "/raw/", 1)
    return url


def _rank_candidates(urls: Sequence[str], path: str) -> list[str]:
    """Order URLs by how likely each is to be a direct download of *path*'s
    file: exact basename match first, then extension match, then a raw-content
    host, then original order."""
    want_name = Path(path).name.lower()
    want_ext = Path(path).suffix.lower()

    def score(u: str) -> tuple:
        low = u.lower()
        base = low.rsplit("/", 1)[-1].split("?")[0]
        host = urllib.parse.urlsplit(low).hostname or ""
        return (
            0 if want_name and base == want_name else 1,
            0 if want_ext and base.endswith(want_ext) else 1,
            0 if any(host.endswith(h) for h in _RAWISH_HOSTS) else 1,
        )

    return [u for _, u in sorted(enumerate(dict.fromkeys(urls)),
                                 key=lambda iu: (score(iu[1]), iu[0]))]


def _cache_name(res: ReferenceResource) -> str:
    stem = Path(res.path).name or hashlib.sha1(res.query().encode()).hexdigest()[:16]
    return stem or "reference.dat"


def fetch(resources: Sequence[ReferenceResource], root: str | Path,
          cache_dir: str | Path, log=print, search_url: str | None = None,
          max_download_tries: int = 3) -> list[ReferenceResource]:
    """Retrieve every resource that isn't already on disk, **via search**.

    For a resource with ``search`` (the intended path), the query is run
    through a search engine; the ranked result URLs are recorded on the
    resource (so the brief can show them) and the harness tries to download
    the best few. A resource with only ``url`` still works — that URL is used
    directly, as a single candidate.

    Fails open at every step: an unreachable search engine, no usable
    results, or a download that 403s all leave the resource without a
    ``local_path`` (the brief then carries the query and candidates for the
    agent to finish), and the run continues.
    """
    root = Path(root)
    cache_dir = Path(cache_dir)
    out: list[ReferenceResource] = []
    for res in resources:
        in_repo = (root / res.path) if res.path else None
        if in_repo and in_repo.is_file() and in_repo.stat().st_size > 0:
            out.append(replace(res, local_path=res.path))
            continue

        candidates: list[str] = []
        if res.search:
            hits = web_search(res.query(), engine=search_url, log=log)
            candidates = [h.url for h in hits]
            log(f"refs: searched {res.query()!r} -> {len(candidates)} candidate(s)")
        if res.url and res.url not in candidates:
            candidates.insert(0, res.url)
        if not candidates:
            out.append(replace(res, candidates=()))
            continue

        ranked = _rank_candidates(candidates, res.path)
        dest = cache_dir / _cache_name(res)
        chosen = ""
        for cand in ranked[:max_download_tries]:
            target = _to_raw_url(cand)
            try:
                cache_dir.mkdir(parents=True, exist_ok=True)
                _download(target, dest)
                chosen = target
                log(f"refs: fetched {res.name} from {target} -> {dest}")
                break
            except (urllib.error.URLError, OSError, ValueError) as exc:
                log(f"refs: {target} did not download ({exc})")
        if not chosen:
            log(f"refs: could not auto-download {res.name}; the brief carries "
                f"the query and {len(ranked)} candidate(s) for the agent")
            out.append(replace(res, candidates=tuple(ranked)))
            continue
        try:
            rel = str(dest.relative_to(root))
        except ValueError:
            rel = str(dest)
        out.append(replace(res, local_path=rel, source_url=chosen, candidates=tuple(ranked)))
    return out


def provenance_for(res: ReferenceResource, file_path: str | Path,
                   *, extra: dict | None = None) -> dict:
    file_path = Path(file_path)
    doc = {
        "artifact": res.name,
        "source_url": res.source_url or res.url,
        "search_query": res.search or None,
        "retrieved_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "sha256": sha256_of(file_path),
        "bytes": file_path.stat().st_size,
        "license": res.license or "unknown",
        "candidates": list(res.candidates),
        "retrieved_by": "graph-agent-harness --fetch-refs (search)",
    }
    doc.update(extra or {})
    return doc


def place_artifact(res: ReferenceResource, root: str | Path, cache_dir: str | Path,
                   *, search_url: str | None = None, log=print) -> ReferenceResource:
    """Retrieve an Artifact node's file to its declared ``path`` and write the
    ``<path>.provenance.json`` sidecar next to it. On success the returned
    resource has ``local_path == res.path``; on failure it carries the search
    ``candidates`` (no ``local_path``) so the node's own agent can finish.

    Idempotent: if the file is already at ``path`` it is left untouched.
    """
    root = Path(root)
    if not res.path:
        return fetch([res], root, cache_dir, log=log, search_url=search_url)[0]
    dest = root / res.path
    if dest.is_file() and dest.stat().st_size:
        return replace(res, local_path=res.path)

    got = fetch([res], root, cache_dir, log=log, search_url=search_url)[0]
    if not got.local_path:
        return got
    src = root / got.local_path
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)
    prov = dest.with_name(dest.name + ".provenance.json")
    if not prov.exists():
        prov.write_text(json.dumps(provenance_for(got, dest), indent=2) + "\n", encoding="utf-8")
    log(f"refs: placed {res.name} at {res.path} (+ provenance)")
    return replace(got, local_path=res.path)


def sha256_of(path: str | Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


@dataclass(frozen=True)
class ArtifactCheck:
    ok: bool
    note: str = ""
    detail: str = ""


def check_artifact(res: ReferenceResource, root: str | Path) -> ArtifactCheck:
    """The deterministic gate for an Artifact node: the declared file must
    exist, be non-empty, and match the pinned sha256 if the graph gave one.
    This stands in for the code-review pass, which has nothing to say about a
    downloaded data file."""
    root = Path(root)
    if not res.path:
        return ArtifactCheck(True, note="no path pinned — cannot verify placement")
    target = root / res.path
    if not target.is_file():
        return ArtifactCheck(
            False, note=f"artifact not found at {res.path}",
            detail=f"The artifact was not placed at its declared path `{res.path}`. "
                   f"Download it from the source and save it there exactly.")
    if target.stat().st_size == 0:
        return ArtifactCheck(False, note=f"artifact at {res.path} is empty",
                             detail=f"`{res.path}` exists but is empty.")
    if res.sha256:
        got = sha256_of(target)
        if got != res.sha256:
            return ArtifactCheck(
                False, note=f"sha256 mismatch for {res.path}",
                detail=f"`{res.path}` sha256 is {got}, the graph pins {res.sha256}. "
                       f"This is the wrong file, a truncated download, or a changed "
                       f"upstream — do not adjust the pin to match; get the right file.")
    return ArtifactCheck(True, note=f"{res.path} present ({target.stat().st_size} bytes)")


def _http(url: str, *, data: bytes | None = None, timeout: int = _TIMEOUT) -> bytes:
    """One GET or POST, http(s) only, capped at _MAX_BYTES. The single network
    primitive — tests patch this."""
    if not url.lower().startswith(("http://", "https://")):
        raise ValueError(f"refusing non-http url: {url}")
    req = urllib.request.Request(url, data=data, headers={
        "User-Agent": _BROWSERISH_UA,
        "Accept": "text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.8",
        "Accept-Language": "en-US,en;q=0.9",
    })
    with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (http(s) enforced above)
        body = resp.read(_MAX_BYTES + 1)
    if len(body) > _MAX_BYTES:
        raise ValueError(f"{url} exceeds the {_MAX_BYTES} byte cap")
    return body


def _main(argv: Sequence[str] | None = None) -> int:
    """`python -m agent_harness.references "<query>"` — show what the harness's
    search engine returns for a query, so a graph's `search` string can be
    tuned by hand."""
    import sys
    args = list(argv if argv is not None else sys.argv[1:])
    if not args:
        print('usage: python -m agent_harness.references "<search query>"', file=sys.stderr)
        return 2
    hits = web_search(" ".join(args))
    if not hits:
        print("(no results — the engine may be unreachable or blocking; try "
              "HARNESS_SEARCH_URL=<a searxng instance>/search?q={query}&format=json)")
        return 1
    for i, h in enumerate(hits, 1):
        print(f"{i:2}. {h.url}")
        if h.title:
            print(f"    {h.title[:110]}")
    return 0


def _looks_like_a_web_page(data: bytes) -> bool:
    head = data[:512].lstrip().lower()
    return head.startswith((b"<!doctype html", b"<html", b"<?xml")) or b"<head" in head


def _download(url: str, dest: Path) -> None:
    data = _http(url)
    # A search result can point at a landing page rather than the file itself;
    # a data file that turns out to be HTML is almost never what was wanted.
    if _looks_like_a_web_page(data) and dest.suffix.lower() not in ("", ".html", ".htm", ".xml"):
        raise ValueError(f"{url} served an HTML page, not the {dest.suffix} file")
    tmp = dest.with_suffix(dest.suffix + ".part")
    tmp.write_bytes(data)
    tmp.replace(dest)


if __name__ == "__main__":
    raise SystemExit(_main())
