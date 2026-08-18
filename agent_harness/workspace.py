"""Git-backed snapshot/restore of the working tree.

The scope fence needs two things a subprocess agent cannot be trusted to
provide: what actually changed on disk, and a way to put back anything that
should not have.  Both are done through git, without writing to the history:
``git stash create`` yields a commit object capturing the tracked state
without touching the worktree, index, or refs; untracked files are backed up
by copy.
"""
from __future__ import annotations

import hashlib
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

from .pathglob import matches_any

ADDED, MODIFIED, DELETED = "added", "modified", "deleted"
#: Untracked files larger than this are recorded but not backed up.
MAX_BACKUP_BYTES = 2 * 1024 * 1024


class WorkspaceError(RuntimeError):
    pass


@dataclass
class Snapshot:
    commit: str
    untracked: dict[str, str] = field(default_factory=dict)   # path -> sha256
    backup_dir: Path | None = None


def run_git(root: Path, *args: str, check: bool = True) -> str:
    proc = subprocess.run(
        ["git", *args], cwd=str(root), capture_output=True, text=True,
    )
    if check and proc.returncode != 0:
        raise WorkspaceError(f"git {' '.join(args)} failed: {proc.stderr.strip()}")
    return proc.stdout


class GitWorkspace:
    """Snapshots and restores a repository working tree."""

    def __init__(self, root: str | Path, ignore: Sequence[str] = ()):
        self.root = Path(root).resolve()
        self.ignore = tuple(ignore)
        if not (self.root / ".git").exists():
            raise WorkspaceError(
                f"{self.root} is not a git repository — the scope fence needs git to "
                f"detect and undo out-of-scope edits (use --no-scope-guard to disable)"
            )

    # ------------------------------------------------------------- snapshots
    def snapshot(self, backup_dir: Path | None = None) -> Snapshot:
        commit = run_git(self.root, "stash", "create").strip()
        if not commit:
            commit = run_git(self.root, "rev-parse", "HEAD").strip()
        untracked: dict[str, str] = {}
        if backup_dir is not None:
            backup_dir.mkdir(parents=True, exist_ok=True)
        for path in self._untracked_paths():
            full = self.root / path
            try:
                data = full.read_bytes()
            except OSError:
                continue
            untracked[path] = hashlib.sha256(data).hexdigest()
            if backup_dir is not None and len(data) <= MAX_BACKUP_BYTES:
                dest = backup_dir / path
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
        return Snapshot(commit=commit, untracked=untracked, backup_dir=backup_dir)

    def _untracked_paths(self) -> list[str]:
        out = run_git(self.root, "status", "--porcelain=v1", "-uall", "--no-renames")
        paths = []
        for line in out.splitlines():
            if line.startswith("?? "):
                p = line[3:].strip().strip('"')
                if not self._ignored(p):
                    paths.append(p)
        return paths

    def _ignored(self, path: str) -> bool:
        return bool(self.ignore) and matches_any(self.ignore, path)

    # --------------------------------------------------------------- changes
    def changes(self, snap: Snapshot) -> dict[str, str]:
        """Repo-relative path -> added|modified|deleted, ignore-list applied."""
        changed: dict[str, str] = {}
        out = run_git(self.root, "diff", "--name-status", "--no-renames", snap.commit)
        for line in out.splitlines():
            parts = line.split("\t", 1)
            if len(parts) != 2:
                continue
            code, path = parts[0].strip(), parts[1].strip()
            if self._ignored(path):
                continue
            changed[path] = {"A": ADDED, "D": DELETED}.get(code[:1], MODIFIED)
        for path in self._untracked_paths():
            before = snap.untracked.get(path)
            if before is None:
                changed[path] = ADDED
            else:
                full = self.root / path
                try:
                    now = hashlib.sha256(full.read_bytes()).hexdigest()
                except OSError:
                    continue
                if now != before:
                    changed[path] = MODIFIED
        for path in snap.untracked:
            if not (self.root / path).exists():
                changed[path] = DELETED
        return dict(sorted(changed.items()))

    def diff(self, snap: Snapshot, paths: Iterable[str] = (), max_chars: int = 60000) -> str:
        """Unified diff since the snapshot, with new untracked files inlined."""
        selected = list(paths)
        args = ["diff", "--no-color", snap.commit]
        if selected:
            args += ["--", *selected]
        text = run_git(self.root, *args, check=False)
        for path in sorted(set(self._untracked_paths()) - set(snap.untracked)):
            if selected and path not in selected:
                continue
            body = self._read_text(self.root / path)
            if body is None:
                continue
            text += f"\n--- /dev/null\n+++ b/{path}\n"
            text += "".join(f"+{line}\n" for line in body.splitlines())
        if len(text) > max_chars:
            text = text[:max_chars] + f"\n... [diff truncated at {max_chars} chars]\n"
        return text

    @staticmethod
    def _read_text(path: Path) -> str | None:
        try:
            return path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            return None

    def added_line_count(self, snap: Snapshot) -> int:
        total = 0
        out = run_git(self.root, "diff", "--numstat", snap.commit, check=False)
        for line in out.splitlines():
            parts = line.split("\t")
            if len(parts) >= 3 and parts[0].isdigit() and not self._ignored(parts[2]):
                total += int(parts[0])
        for path in sorted(set(self._untracked_paths()) - set(snap.untracked)):
            body = self._read_text(self.root / path)
            if body:
                total += len(body.splitlines())
        return total

    # --------------------------------------------------------------- restore
    def restore(self, snap: Snapshot, paths: Iterable[str]) -> list[str]:
        """Put the given paths back the way the snapshot found them."""
        restored: list[str] = []
        for path in paths:
            full = self.root / path
            in_commit = subprocess.run(
                ["git", "cat-file", "-e", f"{snap.commit}:{path}"],
                cwd=str(self.root), capture_output=True,
            ).returncode == 0
            if in_commit:
                run_git(self.root, "checkout", snap.commit, "--", path, check=False)
                run_git(self.root, "reset", "-q", "HEAD", "--", path, check=False)
                restored.append(path)
                continue
            backup = (snap.backup_dir / path) if snap.backup_dir else None
            if backup is not None and backup.exists():
                full.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(backup, full)
                restored.append(path)
                continue
            if full.exists():
                if full.is_dir():
                    shutil.rmtree(full, ignore_errors=True)
                else:
                    full.unlink()
                restored.append(path)
        return restored

    # ---------------------------------------------------------------- commit
    def commit(self, message: str, paths: Iterable[str] = ()) -> str | None:
        paths = list(paths)
        if paths:
            run_git(self.root, "add", "--", *paths, check=False)
        else:
            run_git(self.root, "add", "-A", check=False)
        staged = run_git(self.root, "diff", "--cached", "--name-only", check=False).strip()
        if not staged:
            return None
        run_git(self.root, "commit", "-m", message, check=False)
        return run_git(self.root, "rev-parse", "HEAD", check=False).strip()

    def current_branch(self) -> str:
        return run_git(self.root, "rev-parse", "--abbrev-ref", "HEAD", check=False).strip()
