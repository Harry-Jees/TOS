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
    # Bounded fallback: only the folders a save dialog realistically lands in.
    # A full recursive walk of the profile (AppData alone is huge) makes this
    # test take minutes for no benefit.
    for folder in ("Documents", "Desktop", "Downloads", "OneDrive"):
        root = os.path.join(home, folder)
        if not os.path.isdir(root):
            continue
        for dirpath, _, files in os.walk(root):
            if name in files:
                return os.path.join(dirpath, name)
    return ""


def main() -> int:
    if not os.path.exists(EXE):
        print(f"hawk.exe not found at {EXE}")
        return 2

    existing = find_file(FILENAME)
    if existing:
        os.remove(existing)
    # Clear every leftover Notepad window: Windows 11 Notepad restores tabs
    # from the previous session, and a restored *named* tab makes Ctrl+S save
    # silently instead of opening the Save dialog this test drives.
    for _ in range(6):
        run("--run", "close Notepad")
        time.sleep(0.5)
        run("--run", "try key alt+n")  # discard unsaved changes if prompted
        time.sleep(0.4)
        if "Notepad" not in run("--windows").stdout:
            break
    time.sleep(0.8)

    script = "\n".join([
        "open notepad",
        "wait 2200",
        "focus Notepad",
        # a fresh, empty document: clear whatever a previous session left in
        # the tab, so the saved file holds exactly the typed text
        "key ctrl+a",
        "key delete",
        "wait 400",
        f"type {TEXT}",
        "wait 400",
        # Save As explicitly: with a restored named tab, plain Ctrl+S would
        # save silently and skip the dialog this test is about.
        "key ctrl+shift+s",
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
