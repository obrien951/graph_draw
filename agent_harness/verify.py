"""Running the project's own build/test commands after a node lands."""
from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


@dataclass
class VerifyResult:
    ok: bool
    command: str = ""
    output: str = ""

    def feedback(self, max_chars: int = 6000) -> str:
        tail = self.output[-max_chars:]
        return f"The verification command failed:\n    $ {self.command}\n\n{tail}"


class Verifier:
    """Runs each command in order and stops at the first failure."""

    def __init__(self, commands: Sequence[str], cwd: str | Path, timeout: int = 1800):
        self.commands = tuple(c for c in commands if c.strip())
        self.cwd = Path(cwd)
        self.timeout = timeout

    def enabled(self) -> bool:
        return bool(self.commands)

    def run(self, log_path: Path | None = None) -> VerifyResult:
        for command in self.commands:
            try:
                proc = subprocess.run(
                    command, shell=True, cwd=str(self.cwd),
                    capture_output=True, text=True, timeout=self.timeout,
                )
                output = proc.stdout + proc.stderr
                failed = proc.returncode != 0
            except subprocess.TimeoutExpired:
                output, failed = f"timed out after {self.timeout}s", True
            except OSError as exc:
                output, failed = str(exc), True
            if log_path:
                log_path.parent.mkdir(parents=True, exist_ok=True)
                with log_path.open("a", encoding="utf-8") as fh:
                    fh.write(f"\n$ {command}\n{output}\n")
            if failed:
                return VerifyResult(False, command, output)
        return VerifyResult(True)
