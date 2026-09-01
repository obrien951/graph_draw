"""Ways to actually run an agent.

Two families, one interface:

* **Agentic CLI** (`opencode`) — reads and writes the tree itself. The harness
  hands it a message and lets it work.
* **Completion model** (`llama-server` / any OpenAI-compatible endpoint such as
  the llama-swap box in ~/.config/opencode/opencode.json, or a local `llama-cli`
  with a .gguf) — cannot touch files, so it answers in the file-block protocol
  and the harness applies the blocks.

Both return the same RunResult, so runner.py never asks which one it has.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import urllib.error
import urllib.request
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

from .patchformat import PROTOCOL_HELP, FileBlockApplier, parse_blocks

OPENCODE = "opencode"
LLAMA_SERVER = "llama-server"
LLAMA_CLI = "llama-cli"
DRY_RUN = "dry-run"
BACKENDS = (OPENCODE, LLAMA_SERVER, LLAMA_CLI, DRY_RUN)

DEFAULT_OPENCODE_BIN = str(Path.home() / ".opencode" / "bin" / "opencode")
DEFAULT_SERVER_URL = "http://127.0.0.1:8080/v1"


def _decode(data) -> str:
    """subprocess.TimeoutExpired.stdout/.stderr come back as bytes even under
    text=True — a CPython quirk on that exception path specifically."""
    if not data:
        return ""
    if isinstance(data, bytes):
        return data.decode("utf-8", "replace")
    return data


@dataclass
class AgentSpec:
    """Everything the caller chose about *which* agent runs."""
    backend: str = OPENCODE
    model: str | None = None
    agent: str | None = None
    binary: str | None = None
    server_url: str = DEFAULT_SERVER_URL
    api_key: str | None = None
    gguf: str | None = None
    ctx_size: int = 32768
    max_tokens: int = 8192
    temperature: float = 0.2
    timeout: int = 3600
    variant: str | None = None
    extra_args: tuple[str, ...] = ()

    def label(self) -> str:
        bits = [self.backend]
        if self.model:
            bits.append(self.model)
        if self.agent:
            bits.append(f"agent={self.agent}")
        if self.gguf:
            bits.append(Path(self.gguf).name)
        return " ".join(bits)


@dataclass
class RunRequest:
    prompt: str
    cwd: Path
    label: str
    log_path: Path | None = None
    system: str | None = None
    writes_files: bool = True      # False for review-only calls
    continue_session: bool = False  # follow up in the same session, not a fresh one


@dataclass
class RunResult:
    ok: bool
    text: str = ""
    error: str = ""
    exit_code: int = 0
    written: list[str] = field(default_factory=list)


class Backend(ABC):
    """Runs one prompt. Implementations differ only in how the work lands."""

    #: True when the backend edits the working tree by itself.
    agentic = True
    #: False for backends that only inspect (dry runs, reviewers).
    mutates = True
    #: True when RunRequest.continue_session means something to this backend
    #: (a real, resumable conversation) rather than being silently ignored.
    supports_continue = False

    def __init__(self, spec: AgentSpec):
        self.spec = spec

    @abstractmethod
    def run(self, request: RunRequest) -> RunResult: ...

    def describe(self) -> str:
        return self.spec.label()

    def prompt_suffix(self, request: RunRequest) -> str:
        """Extra instructions this backend needs appended to the prompt."""
        return "" if self.agentic or not request.writes_files else "\n" + PROTOCOL_HELP

    @staticmethod
    def _log(request: RunRequest, text: str) -> None:
        if request.log_path:
            request.log_path.parent.mkdir(parents=True, exist_ok=True)
            # Each attempt already gets its own numbered log path, so append
            # mode only served to glue a stale run's log onto today's - e.g. a
            # leftover llama-cli invocation line ahead of the real opencode one.
            mode = "a" if getattr(request, "_log_opened", False) else "w"
            with request.log_path.open(mode, encoding="utf-8") as fh:
                fh.write(text)
            request._log_opened = True


class OpencodeBackend(Backend):
    """`opencode run` — an agentic CLI that edits the repository itself."""

    agentic = True
    supports_continue = True

    def __init__(self, spec: AgentSpec):
        super().__init__(spec)
        self.binary = spec.binary or shutil.which("opencode") or DEFAULT_OPENCODE_BIN
        if not Path(self.binary).exists() and not shutil.which(self.binary):
            raise FileNotFoundError(
                f"opencode not found at {self.binary!r} — pass --agent-binary"
            )

    def _argv(self, request: RunRequest) -> list[str]:
        argv = [self.binary, "run", "--dir", str(request.cwd)]
        # -c resumes opencode's own "last session" for this directory rather
        # than starting a fresh one, so a follow-up prompt lands in the SAME
        # conversation as the attempt it is checking on, not a blank context
        # that has to be told everything again.
        if request.continue_session:
            argv.append("-c")
        if self.spec.model:
            argv += ["-m", self.spec.model]
        if self.spec.agent:
            argv += ["--agent", self.spec.agent]
        if self.spec.variant:
            argv += ["--variant", self.spec.variant]
        argv += ["--auto", "--title", request.label[:60]]
        argv += list(self.spec.extra_args)
        argv.append(request.prompt)
        return argv

    def run(self, request: RunRequest) -> RunResult:
        argv = self._argv(request)
        self._log(request, f"$ {argv[0]} run --dir {request.cwd} "
                           f"{' '.join(argv[3:-1])} <prompt {len(request.prompt)} chars>\n\n")
        try:
            proc = subprocess.run(
                argv, cwd=str(request.cwd), capture_output=True, text=True,
                timeout=self.spec.timeout,
            )
        except subprocess.TimeoutExpired as exc:
            # subprocess still captures whatever the child had written to its
            # pipes before the kill (as bytes even though text=True — a CPython
            # quirk on this exception path), which is otherwise the only
            # record of what the agent was doing when it got killed. Without
            # this, a timeout leaves a completely empty log: no way to tell a
            # stuck compression/summarization loop from a slow-but-progressing
            # turn from a genuine hang.
            partial = _decode(exc.stdout) + _decode(exc.stderr)
            if partial:
                self._log(request, partial)
            return RunResult(False, error=f"opencode timed out after {self.spec.timeout}s", exit_code=124)
        except OSError as exc:
            return RunResult(False, error=f"cannot run opencode: {exc}", exit_code=127)
        out = proc.stdout + ("\n" + proc.stderr if proc.stderr else "")
        self._log(request, out)
        return RunResult(proc.returncode == 0, text=out,
                         error="" if proc.returncode == 0 else proc.stderr.strip()[-2000:],
                         exit_code=proc.returncode)


class TextBackend(Backend):
    """Base for completion models: get text, apply file blocks."""

    agentic = False

    def __init__(self, spec: AgentSpec, applier: FileBlockApplier | None = None):
        super().__init__(spec)
        self.applier = applier

    @abstractmethod
    def complete(self, request: RunRequest) -> RunResult: ...

    def run(self, request: RunRequest) -> RunResult:
        result = self.complete(request)
        if not result.ok or not request.writes_files:
            return result
        if self.applier is None:
            return RunResult(False, text=result.text,
                             error="no file applier configured for a text backend")
        blocks = parse_blocks(result.text)
        if not blocks:
            return RunResult(False, text=result.text,
                             error="model returned no <<<FILE ...>>> blocks")
        written, errors = self.applier.apply(blocks)
        result.written = written
        if errors:
            result.ok = False
            result.error = "; ".join(errors)
        return result


class OpenAICompatBackend(TextBackend):
    """Chat completions over HTTP: llama-server, llama-swap, any OpenAI-shaped API."""

    def __init__(self, spec: AgentSpec, applier: FileBlockApplier | None = None):
        super().__init__(spec, applier)
        self.url = spec.server_url.rstrip("/")
        if not self.url.endswith("/v1"):
            self.url += "/v1"

    def complete(self, request: RunRequest) -> RunResult:
        messages = []
        if request.system:
            messages.append({"role": "system", "content": request.system})
        messages.append({"role": "user", "content": request.prompt})
        payload = {
            "model": self.spec.model or "local",
            "messages": messages,
            "temperature": self.spec.temperature,
            "max_tokens": self.spec.max_tokens,
            "stream": False,
        }
        key = self.spec.api_key or os.environ.get("OPENAI_API_KEY", "not-needed")
        req = urllib.request.Request(
            f"{self.url}/chat/completions",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {key}"},
            method="POST",
        )
        self._log(request, f"POST {self.url}/chat/completions model={payload['model']} "
                           f"prompt={len(request.prompt)} chars\n\n")
        try:
            with urllib.request.urlopen(req, timeout=self.spec.timeout) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", "replace")[:2000]
            return RunResult(False, error=f"HTTP {exc.code} from {self.url}: {body}", exit_code=1)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
            return RunResult(False, error=f"{self.url}: {exc}", exit_code=1)
        try:
            text = data["choices"][0]["message"]["content"] or ""
        except (KeyError, IndexError, TypeError):
            return RunResult(False, error=f"unexpected response shape: {str(data)[:500]}")
        self._log(request, text)
        return RunResult(True, text=text)


class LlamaCliBackend(TextBackend):
    """A local .gguf through llama.cpp's `llama-cli`, one turn, no chat loop."""

    def __init__(self, spec: AgentSpec, applier: FileBlockApplier | None = None):
        super().__init__(spec, applier)
        self.binary = spec.binary or shutil.which("llama-cli") or "llama-cli"
        self.gguf = spec.gguf or spec.model
        if not self.gguf:
            raise ValueError("llama-cli needs a model file: pass --gguf /path/to/model.gguf")

    def complete(self, request: RunRequest) -> RunResult:
        prompt_file = (request.log_path or request.cwd / "prompt").with_suffix(".prompt.txt")
        prompt_file.parent.mkdir(parents=True, exist_ok=True)
        prompt_file.write_text(request.prompt, encoding="utf-8")
        argv = [
            self.binary, "-m", self.gguf, "-f", str(prompt_file),
            "-c", str(self.spec.ctx_size), "-n", str(self.spec.max_tokens),
            "--temp", str(self.spec.temperature),
            "--no-conversation", "-st", "--simple-io", "--no-display-prompt", "--no-warmup",
        ]
        if request.system:
            argv += ["-sys", request.system]
        argv += list(self.spec.extra_args)
        self._log(request, f"$ {' '.join(argv)}\n\n")
        try:
            proc = subprocess.run(argv, capture_output=True, text=True, timeout=self.spec.timeout)
        except subprocess.TimeoutExpired as exc:
            partial = _decode(exc.stdout) + _decode(exc.stderr)
            if partial:
                self._log(request, partial)
            return RunResult(False, error=f"llama-cli timed out after {self.spec.timeout}s", exit_code=124)
        except OSError as exc:
            return RunResult(False, error=f"cannot run llama-cli: {exc}", exit_code=127)
        self._log(request, proc.stdout)
        if proc.returncode != 0:
            return RunResult(False, text=proc.stdout, error=proc.stderr.strip()[-2000:],
                             exit_code=proc.returncode)
        return RunResult(True, text=proc.stdout)


class DryRunBackend(Backend):
    """Writes the brief that would have been sent and changes nothing."""

    agentic = True
    mutates = False

    def run(self, request: RunRequest) -> RunResult:
        self._log(request, request.prompt)
        return RunResult(True, text=f"[dry-run] {request.label}: "
                                    f"{len(request.prompt)} chars of prompt, no agent launched")

    def describe(self) -> str:
        return "dry-run (no agent launched)"


def build_backend(spec: AgentSpec, root: str | Path) -> Backend:
    applier = FileBlockApplier(root)
    if spec.backend == OPENCODE:
        return OpencodeBackend(spec)
    if spec.backend == LLAMA_SERVER:
        return OpenAICompatBackend(spec, applier)
    if spec.backend == LLAMA_CLI:
        return LlamaCliBackend(spec, applier)
    if spec.backend == DRY_RUN:
        return DryRunBackend(spec)
    raise ValueError(f"unknown backend {spec.backend!r}; expected one of {BACKENDS}")
