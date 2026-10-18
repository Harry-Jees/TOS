#!/usr/bin/env python3
"""Non-interactive safety regression tests; does not move the real desktop."""
from __future__ import annotations

import os
import secrets
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "hawk.exe")
WORK = os.path.join(ROOT, "workspace")


def run_script(script: str, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run([EXE, *extra, "--run", script], capture_output=True, text=True,
                          timeout=30, errors="replace")


def main() -> int:
    if not os.path.isfile(EXE):
        print(f"hawk.exe not found: {EXE}")
        return 2
    os.makedirs(WORK, exist_ok=True)
    failures = 0

    # Path traversal must be rejected before a file is created.
    escape_name = "hawk-security-escape-" + secrets.token_hex(6) + ".txt"
    outside = os.path.join(ROOT, escape_name)
    result = run_script("write ..\\" + escape_name + " must-not-exist")
    passed = result.returncode != 0 and not os.path.exists(outside)
    print(f"workspace traversal rejected: {'PASS' if passed else 'FAIL'}")
    failures += not passed
    if os.path.exists(outside):
        os.remove(outside)

    # Writes must not silently truncate a file already in the workspace.
    existing = os.path.join(WORK, "hawk-security-overwrite.txt")
    with open(existing, "w", encoding="utf-8") as f:
        f.write("original")
    result = run_script("write hawk-security-overwrite.txt changed")
    with open(existing, "r", encoding="utf-8") as f:
        content = f.read()
    passed = result.returncode != 0 and content == "original"
    print(f"existing workspace file protected: {'PASS' if passed else 'FAIL'}")
    failures += not passed
    os.remove(existing)

    # Ensure successful file writes remain possible for normal tasks.
    created = os.path.join(WORK, "hawk-security-created.txt")
    if os.path.exists(created):
        os.remove(created)
    result = run_script("write hawk-security-created.txt safe")
    try:
        with open(created, "r", encoding="utf-8") as f:
            content = f.read()
    except OSError:
        content = None
    passed = result.returncode == 0 and content == "safe"
    print(f"new workspace file can be created: {'PASS' if passed else 'FAIL'}")
    failures += not passed
    if os.path.exists(created):
        os.remove(created)

    # Destructive commands require GUI approval or an explicit CLI override.
    guarded = os.path.join(WORK, "hawk-security-approval.txt")
    with open(guarded, "w", encoding="utf-8") as f:
        f.write("keep-me")
    result = run_script("del hawk-security-approval.txt")
    denied = result.returncode != 0 and os.path.exists(guarded) and "not approved" in (
        result.stdout + result.stderr).lower()
    print(f"sensitive delete denied by default: {'PASS' if denied else 'FAIL'}")
    failures += not denied
    result = run_script("del hawk-security-approval.txt", "--allow-sensitive")
    allowed = result.returncode == 0 and not os.path.exists(guarded)
    print(f"explicit sensitive-action override works: {'PASS' if allowed else 'FAIL'}")
    failures += not allowed
    if os.path.exists(guarded):
        os.remove(guarded)

    print(f"\n{5 - failures}/5 safety checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
