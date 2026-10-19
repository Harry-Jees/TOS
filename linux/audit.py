#!/usr/bin/env python3
"""Read-only Linux host and workspace audit for Hawk OS. Python 3.10+, stdlib only."""
from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


def command_version(command: str, args: list[str] | None = None) -> dict:
    executable = shutil.which(command)
    if not executable:
        return {"available": False}
    try:
        result = subprocess.run(
            [executable, *(args or ["--version"])],
            capture_output=True, text=True, timeout=3, check=False,
        )
        # Keep output short and avoid exposing arbitrary command output.
        first_line = (result.stdout or result.stderr).strip().splitlines()
        return {"available": True, "path": executable,
                "version": first_line[0][:240] if first_line else "version unavailable",
                "exit_code": result.returncode}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"available": True, "path": executable, "error": type(exc).__name__}


def memory_summary() -> dict:
    result = {}
    try:
        for line in Path("/proc/meminfo").read_text(errors="replace").splitlines():
            key, _, value = line.partition(":")
            if key in {"MemTotal", "MemAvailable", "SwapTotal", "SwapFree"}:
                parts = value.strip().split()
                if parts:
                    result[key] = int(parts[0]) * 1024
    except OSError:
        pass
    return result


def process_summary(limit: int = 80) -> dict:
    # Do not collect command-line arguments: they can contain passwords/tokens.
    try:
        entries = []
        for proc in Path("/proc").iterdir():
            if not proc.name.isdigit():
                continue
            try:
                raw = (proc / "comm").read_text(errors="replace").strip()
                status = {}
                for line in (proc / "status").read_text(errors="replace").splitlines():
                    if line.startswith(("Name:", "State:", "Uid:")):
                        k, _, v = line.partition(":")
                        status[k] = v.strip()
                entries.append({"pid": int(proc.name), "name": raw[:120], **status})
            except (OSError, PermissionError):
                continue
        return {"visible_count": len(entries), "items": entries[:limit],
                "truncated": len(entries) > limit}
    except OSError:
        return {"available": False, "items": []}


def git_summary(path: Path) -> dict:
    git = shutil.which("git")
    if not git:
        return {"available": False}
    try:
        root = subprocess.run([git, "-C", str(path), "rev-parse", "--show-toplevel"],
                              capture_output=True, text=True, timeout=3, check=False)
        if root.returncode:
            return {"repository": False}
        branch = subprocess.run([git, "-C", str(path), "branch", "--show-current"],
                                capture_output=True, text=True, timeout=3, check=False)
        status = subprocess.run([git, "-C", str(path), "status", "--short"],
                                capture_output=True, text=True, timeout=3, check=False)
        return {"repository": True, "root": root.stdout.strip(),
                "branch": branch.stdout.strip(),
                "dirty": bool(status.stdout.strip()),
                "changed_path_count": len(status.stdout.splitlines())}
    except (OSError, subprocess.TimeoutExpired):
        return {"available": True, "error": "git command failed or timed out"}


def audit(target: Path) -> dict:
    target = target.expanduser().resolve()
    disk = shutil.disk_usage(target if target.exists() else Path.cwd())
    return {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "mode": "read-only",
        "scope": str(target),
        "host": {
            "system": platform.system(), "release": platform.release(),
            "machine": platform.machine(), "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
            "memory_bytes": memory_summary(),
            "disk_bytes": {"total": disk.total, "used": disk.used, "free": disk.free},
        },
        "tools": {
            "git": git_summary(target),
            "docker": command_version("docker", ["--version"]),
            "podman": command_version("podman", ["--version"]),
            "ollama": command_version("ollama", ["--version"]),
            "llama_cpp_server": {"available": bool(shutil.which("llama-server"))},
        },
        "processes": process_summary(),
        "notes": [
            "Read-only inventory; no processes were stopped and no files changed.",
            "Only information visible to this user's permissions is reported.",
            "Process command-line arguments are intentionally excluded to reduce secret exposure.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", default=".", help="workspace to inspect (default: current directory)")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()
    report = audit(Path(args.path))
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        host = report["host"]
        print("Hawk Linux Audit — read-only")
        print(f"Scope: {report['scope']}")
        print(f"Host: {host['system']} {host['release']} ({host['machine']})")
        print(f"Python: {host['python']} | CPUs: {host['cpu_count']}")
        mem = host["memory_bytes"]
        if "MemTotal" in mem:
            print(f"Memory: {mem['MemTotal'] // (1024**3)} GiB total; "
                  f"{mem.get('MemAvailable', 0) // (1024**3)} GiB available")
        print(f"Disk free: {host['disk_bytes']['free'] // (1024**3)} GiB")
        for name, item in report["tools"].items():
            print(f"{name}: {'available' if item.get('available') else 'not available'}")
        git = report["tools"]["git"]
        if git.get("repository"):
            print(f"Git: {git['branch']} ({'dirty' if git['dirty'] else 'clean'})")
        print(f"Visible processes: {report['processes'].get('visible_count', 0)}")
        print("No actions were executed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
