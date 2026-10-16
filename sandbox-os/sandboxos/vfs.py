"""Jailed virtual filesystem for SandboxOS.

This is the sandbox's disk. It is a single directory on the host, but every
path the sandbox sees ("/etc/motd", "/home/user/notes.txt") is resolved and
*verified* to stay inside that directory. There is no API here that can touch
a host path, and the shell has no host subprocess to escape through.

Defences:
  * path normalisation rejects any escape attempt (leading "..", absolute host
    paths, NUL bytes, reserved device names)
  * realpath containment check stops symlink-based escapes
  * byte quota, per-file size cap and inode budget
"""

from __future__ import annotations

import fnmatch
import pathlib
import re
import time

RESERVED_HOST_NAMES = {
    "con", "prn", "aux", "nul",
    *(f"com{i}" for i in range(1, 10)),
    *(f"lpt{i}" for i in range(1, 10)),
}


class VfsError(Exception):
    """Raised for any illegal or failed filesystem operation."""


class QuotaExceeded(VfsError):
    pass


def normalize(path: str) -> str:
    """Turn any input into a canonical sandbox-absolute path or raise.

    ".." is resolved *within* the sandbox, like chroot: /home/user/../.. is
    "/". A ".." that would climb above the root is an explicit escape
    attempt and is refused rather than silently clamped, so nobody can
    believe they reached the host filesystem.
    """
    if not isinstance(path, str):
        raise VfsError("path must be a string")
    if "\x00" in path:
        raise VfsError("path contains a NUL byte")
    raw = path.strip().replace("\\", "/")
    if re.match(r"^[A-Za-z]:", raw):
        raise VfsError("drive-letter paths do not exist in SandboxOS")
    if raw.startswith("//"):
        raise VfsError("UNC paths do not exist in SandboxOS")
    if len(raw) > 400:
        raise VfsError("path too long")
    parts: list[str] = []
    for comp in raw.split("/"):
        if comp in ("", "."):
            continue
        if comp == "..":
            if not parts:
                raise VfsError("path escapes the sandbox root")
            parts.pop()
            continue
        if comp.lower() in RESERVED_HOST_NAMES:
            raise VfsError(f"reserved name: {comp}")
        parts.append(comp)
    return "/" + "/".join(parts)


class Vfs:
    def __init__(
        self,
        root: pathlib.Path,
        quota_bytes: int = 8 * 1024 * 1024,
        max_file_bytes: int = 1024 * 1024,
        max_entries: int = 2000,
    ):
        self.root = pathlib.Path(root).resolve()
        self.quota_bytes = quota_bytes
        self.max_file_bytes = max_file_bytes
        self.max_entries = max_entries
        self.root.mkdir(parents=True, exist_ok=True)

    # -- resolution --------------------------------------------------------
    def real(self, path: str) -> pathlib.Path:
        """Resolve a sandbox path to a host path, guaranteed inside root."""
        norm = normalize(path)
        if norm == "/":
            return self.root
        parts = [p for p in norm.split("/") if p]
        target = self.root.joinpath(*parts)
        resolved = pathlib.Path(target).resolve()
        if resolved != self.root and self.root not in resolved.parents:
            raise VfsError("path escapes the sandbox root")
        if resolved != self.root and self.root in pathlib.Path(target).parents:
            # a parent component is a symlink pointing outside; resolve() already
            # caught the outside case above, so this is only defensive
            pass
        return resolved

    def rel(self, host_path: pathlib.Path) -> str:
        try:
            rel = host_path.resolve().relative_to(self.root)
        except ValueError as exc:  # pragma: no cover - defensive
            raise VfsError("path escapes the sandbox root") from exc
        return "/" + rel.as_posix() if str(rel) != "." else "/"

    # -- accounting --------------------------------------------------------
    def usage(self) -> dict:
        files = dirs = 0
        total = 0
        for p in self.root.rglob("*"):
            try:
                if p.is_dir():
                    dirs += 1
                else:
                    files += 1
                    total += p.stat().st_size
            except OSError:
                continue
        return {
            "used_bytes": total,
            "quota_bytes": self.quota_bytes,
            "files": files,
            "dirs": dirs,
            "entries": files + dirs,
            "max_entries": self.max_entries,
        }

    def _check_quota(self, extra_bytes: int) -> None:
        if extra_bytes > self.max_file_bytes:
            raise QuotaExceeded(
                f"file exceeds the {self.max_file_bytes} byte per-file limit"
            )
        used = self.usage()
        if used["entries"] >= self.max_entries:
            raise QuotaExceeded(
                f"inode budget reached ({self.max_entries} entries)"
            )
        if used["used_bytes"] + extra_bytes > self.quota_bytes:
            raise QuotaExceeded(
                f"disk quota exceeded ({self.quota_bytes} bytes total)"
            )

    # -- queries -----------------------------------------------------------
    def exists(self, path: str) -> bool:
        try:
            return self.real(path).exists()
        except VfsError:
            return False

    def is_dir(self, path: str) -> bool:
        return self.real(path).is_dir()

    def is_file(self, path: str) -> bool:
        return self.real(path).is_file()

    def stat(self, path: str) -> dict:
        p = self.real(path)
        if not p.exists():
            raise VfsError(f"no such file or directory: {normalize(path)}")
        st = p.stat()
        return {
            "path": self.rel(p),
            "type": "dir" if p.is_dir() else "file",
            "size": 0 if p.is_dir() else st.st_size,
            "mtime": int(st.st_mtime),
        }

    def list_dir(self, path: str) -> list[dict]:
        p = self.real(path)
        if not p.exists():
            raise VfsError(f"no such file or directory: {normalize(path)}")
        if not p.is_dir():
            raise VfsError(f"not a directory: {normalize(path)}")
        out = []
        for child in sorted(p.iterdir(), key=lambda c: (c.is_file(), c.name.lower())):
            if child.is_symlink():
                continue
            try:
                st = child.stat()
            except OSError:
                continue
            out.append(
                {
                    "name": child.name,
                    "path": self.rel(child),
                    "type": "dir" if child.is_dir() else "file",
                    "size": 0 if child.is_dir() else st.st_size,
                    "mtime": int(st.st_mtime),
                }
            )
        return out

    # -- reads -------------------------------------------------------------
    def read_bytes(self, path: str, limit: int | None = None) -> bytes:
        p = self.real(path)
        if p.is_dir():
            raise VfsError(f"is a directory: {normalize(path)}")
        if not p.is_file():
            raise VfsError(f"no such file: {normalize(path)}")
        data = p.read_bytes()
        return data[:limit] if limit else data

    def read_text(self, path: str, limit: int = 200_000) -> str:
        return self.read_bytes(path, limit).decode("utf-8", errors="replace")

    # -- writes ------------------------------------------------------------
    def write_text(self, path: str, text: str, append: bool = False) -> dict:
        if not isinstance(text, str):
            raise VfsError("content must be a string")
        data = text.encode("utf-8")
        norm = normalize(path)
        p = self.real(norm)
        if p.is_dir():
            raise VfsError(f"is a directory: {norm}")
        old_size = p.stat().st_size if p.exists() else 0
        final_size = old_size + len(data) if append else len(data)
        self._check_quota(final_size - old_size if append else final_size)
        if p.parent and not p.parent.exists():
            self.mkdir(self.rel(p.parent))
        mode = "ab" if append else "wb"
        with p.open(mode) as fh:
            fh.write(data)
        return {"path": norm, "bytes_written": len(data), "size": final_size}

    def mkdir(self, path: str, parents: bool = True) -> dict:
        p = self.real(path)
        if p.exists() and p.is_dir():
            return {"path": normalize(path), "created": False}
        if p.exists():
            raise VfsError(f"already exists and is a file: {normalize(path)}")
        missing = 0
        probe = p
        while not probe.exists() and probe != self.root:
            missing += 1
            probe = probe.parent
        self._check_quota(0)
        if missing and self.usage()["entries"] + missing > self.max_entries:
            raise QuotaExceeded(f"inode budget reached ({self.max_entries})")
        p.mkdir(parents=parents, exist_ok=True)
        return {"path": normalize(path), "created": True}

    # -- mutation ----------------------------------------------------------
    def remove(self, path: str, recursive: bool = False) -> dict:
        p = self.real(path)
        if p == self.root:
            raise VfsError("refusing to remove the sandbox root")
        if not p.exists():
            raise VfsError(f"no such file or directory: {normalize(path)}")
        if p.is_dir():
            if any(p.iterdir()) and not recursive:
                raise VfsError(f"directory not empty (use -r): {normalize(path)}")
            count = sum(1 for _ in p.rglob("*"))
            for child in sorted(p.rglob("*"), reverse=True):
                try:
                    child.unlink() if child.is_file() else child.rmdir()
                except OSError:
                    pass
            p.rmdir()
            return {"path": normalize(path), "removed": count + 1}
        p.unlink()
        return {"path": normalize(path), "removed": 1}

    def move(self, src: str, dst: str) -> dict:
        s, d = self.real(src), self.real(dst)
        if not s.exists():
            raise VfsError(f"no such file or directory: {normalize(src)}")
        if d.exists() and d.is_dir() and s.is_file():
            d = d / s.name
        if d.exists():
            raise VfsError(f"destination exists: {normalize(dst)}")
        if not d.parent.exists():
            self.mkdir(self.rel(d.parent))
        if s.is_file():
            self._check_quota(s.stat().st_size)
        s.rename(d)
        return {"from": normalize(src), "to": self.rel(d)}

    def copy(self, src: str, dst: str) -> dict:
        s, d = self.real(src), self.real(dst)
        if not s.is_file():
            raise VfsError(f"only files can be copied: {normalize(src)}")
        if d.exists() and d.is_dir():
            d = d / s.name
        data = s.read_bytes()
        self.write_text(self.rel(d), data.decode("utf-8", errors="replace"))
        return {"from": normalize(src), "to": self.rel(d), "bytes": len(data)}

    def search(
        self,
        pattern: str,
        path: str = "/",
        ignore_case: bool = True,
        max_results: int = 100,
    ) -> list[dict]:
        flags = re.IGNORECASE if ignore_case else 0
        try:
            rx = re.compile(pattern, flags)
        except re.error as exc:
            raise VfsError(f"bad pattern: {exc}") from exc
        base = self.real(path)
        hits: list[dict] = []
        targets = [base] if base.is_file() else sorted(base.rglob("*"))
        for p in targets:
            if len(hits) >= max_results:
                break
            if not p.is_file():
                continue
            try:
                text = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for i, line in enumerate(text.splitlines(), 1):
                if rx.search(line):
                    hits.append(
                        {
                            "path": self.rel(p),
                            "line": i,
                            "text": line[:200],
                        }
                    )
                    if len(hits) >= max_results:
                        break
        return hits

    def find(self, path: str = "/", name_glob: str = "*", dirs: bool = False) -> list[dict]:
        base = self.real(path)
        out = []
        for p in sorted(base.rglob("*")):
            if not dirs and not p.is_file():
                continue
            if fnmatch.fnmatch(p.name.lower(), name_glob.lower()):
                out.append(self.stat(self.rel(p)))
        return out

    def tree(self, path: str = "/", depth: int = 3) -> str:
        base = self.real(path)
        base_depth = len(base.parts)
        lines: list[str] = [self.rel(base)]
        for p in sorted(base.rglob("*")):
            d = len(p.parts) - base_depth
            if d > depth:
                continue
            indent = "  " * (d - 1) + ("|- " if p.is_file() else "+- ")
            size = f" ({p.stat().st_size} B)" if p.is_file() else "/"
            lines.append(f"{indent}{p.name}{size}")
        return "\n".join(lines)

    def seed(self, files: dict[str, str], dirs: list[str] | None = None) -> None:
        for d in dirs or []:
            self.mkdir(d)
        for path, content in files.items():
            if not self.exists(path):
                self.write_text(path, content)
