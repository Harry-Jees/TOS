"""Append-only audit log (JSONL) kept OUTSIDE the sandbox disk.

Every privileged action - whether taken by the human at the console or by the
AI agent - is recorded here so the sandbox can be reviewed after the fact.
"""

from __future__ import annotations

import json
import re
import threading
import time
import pathlib

_SECRET_RE = re.compile(
    r"(nvapi-[A-Za-z0-9_\-]{8,}|sk-[A-Za-z0-9_\-]{8,}|Bearer\s+[A-Za-z0-9\-._~+/]{8,})"
)


def redact(text: str) -> str:
    """Strip anything that looks like an API key or bearer token."""
    return _SECRET_RE.sub("<redacted>", str(text))


def _clip(value, limit: int = 400):
    if isinstance(value, str):
        return redact(value)[:limit]
    if isinstance(value, (int, float, bool)) or value is None:
        return value
    if isinstance(value, (list, tuple)):
        return [_clip(v, limit) for v in value[:20]]
    if isinstance(value, dict):
        return {str(k): _clip(v, limit) for k, v in list(value.items())[:20]}
    return redact(repr(value))[:limit]


class AuditLog:
    def __init__(self, path: pathlib.Path, actor: str = "system"):
        self.path = pathlib.Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.actor = actor
        self._lock = threading.Lock()
        self._seq = 0

    def record(self, action: str, *, actor: str | None = None, **fields) -> dict:
        with self._lock:
            self._seq += 1
            entry = {
                "seq": self._seq,
                "ts": round(time.time(), 3),
                "actor": actor or self.actor,
                "action": action,
            }
            entry.update({k: _clip(v) for k, v in fields.items()})
            line = json.dumps(entry, ensure_ascii=False)
            try:
                with self.path.open("a", encoding="utf-8") as fh:
                    fh.write(line + "\n")
            except OSError:
                pass  # auditing must never break the sandbox
            return entry

    def tail(self, n: int = 50) -> list[dict]:
        try:
            lines = self.path.read_text(encoding="utf-8").splitlines()[-n:]
        except OSError:
            return []
        out = []
        for line in lines:
            try:
                out.append(json.loads(line))
            except ValueError:
                continue
        return out
