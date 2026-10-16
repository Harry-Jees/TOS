#!/usr/bin/env python3
"""Opt-in check: drive a real third-party application, not just our own code.

Hawk OS opens Notepad, focuses it, types with synthetic keystrokes, saves the
document through Notepad's own Save dialog, and this script then reads the file
back from disk. Nothing is mocked: a separate process did all of it.

This drives the live desktop, so it is not part of the default suite.

    python tests/test_real_app.py
"""

from __future__ import annotations

import os
import glob
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_hawk import EXE, SHOTS, check, run, results  # noqa: E402

FILENAME = "hawk-agent-proof.txt"
TEXT = "Hawk OS wrote this file with real synthetic keystrokes."


def find_file(name: str) -> str:
    home = os.path.expanduser("~")
    for folder in ("Documents", "OneDrive\\Documents", "OneDrive\\Desktop", "Desktop", "."):
        candidate = os.path.join(home, folder, name)
        if os.path.exists(candidate):
            return candidate
    hits = glob.glob(os.path.join(home, "**", name), recursive=True)
    return hits[0] if hits else ""


def main() -> int:
    if not os.path.exists(EXE):
        print(f"hawk.exe not found at {EXE}")
        return 2

    existing = find_file(FILENAME)
    if existing:
        os.remove(existing)
    run("--run", "close Notepad")  # clear any leftover window
    time.sleep(0.5)

    script = "\n".join([
        "open notepad",
        "wait 2200",
        "focus Notepad",
        f"type {TEXT}",
        "wait 400",
        "key ctrl+s",
        "wait 1800",
        f"type {FILENAME}",
        "key enter",
        "wait 1800",
        "shot real-app-proof.png",
    ])
    r = run("--run", script, timeout=240)
    out = r.stdout
    typed_lines = [l for l in out.splitlines() if "typed" in l and TEXT in l]
    check("notepad was opened and driven by the agent",
          r.returncode == 0 and bool(typed_lines),
          (typed_lines or [f"exit {r.returncode}"])[0].strip())
    check("the agent found Notepad by typing its window title",
          "focused \"Untitled - Notepad\"" in out or 'focused "' in out,
          [l for l in out.splitlines() if "focused" in l][:1])

    saved = ""
    deadline = time.time() + 12
    while time.time() < deadline:
        saved = find_file(FILENAME)
        if saved:
            break
        time.sleep(0.4)

    check("Notepad saved the file the agent typed", bool(saved), saved or "file never appeared")
    if saved:
        content = open(saved, encoding="utf-8", errors="replace").read().strip()
        check("the saved file holds exactly the synthetic keystrokes", content == TEXT,
              repr(content[:90]))
    else:
        # leave no modal dialog behind if the save did not land
        run("--run", "key esc")

    shot = os.path.join(SHOTS, "real-app-proof.png")
    check("a screenshot of the result was captured", os.path.exists(shot),
          f"{os.path.getsize(shot)} bytes" if os.path.exists(shot) else "missing")

    run("--run", "close Notepad")
    time.sleep(0.6)

    passed = sum(1 for _, ok, _ in results if ok)
    print(f"\n{passed}/{len(results)} checks passed")
    if passed != len(results):
        print("\nfailures:")
        for name, ok, detail in results:
            if not ok:
                print(f"  - {name}: {detail}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
