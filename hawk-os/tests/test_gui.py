#!/usr/bin/env python3
"""Live checks for the Hawk OS app window.

Starts the real app, reads the geometry it publishes, verifies from captured
pixels that it actually rendered, then physically clicks one of its buttons
with Hawk OS's own input engine and confirms the button did its job.

    python tests/test_gui.py
"""

from __future__ import annotations

import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_hawk import EXE, ROOT, SHOTS, check, desktop, png_pixel, read_png, results, run  # noqa: E402

LAYOUT = os.path.join(ROOT, "ui-layout.txt")
LOG = os.path.join(ROOT, "hawk.log")


def parse_layout(path: str) -> dict[str, tuple[int, int, int, int]]:
    out: dict[str, tuple[int, int, int, int]] = {}
    for line in open(path, encoding="utf-8"):
        if line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) == 5 and all(p.lstrip("-").isdigit() for p in parts[1:]):
            out[parts[0]] = tuple(int(p) for p in parts[1:])  # type: ignore[assignment]
    return out


def count_colour(rgba, w, bpp, rect, want, tol=12) -> int:
    x, y, rw, rh = rect
    n = 0
    for yy in range(y, y + rh):
        for xx in range(x, x + rw):
            px = png_pixel(rgba, w, xx, yy, bpp)
            if all(abs(px[i] - want[i]) <= tol for i in range(3)):
                n += 1
    return n


def distinct_colours(rgba, w, bpp, rect, cap=60) -> int:
    x, y, rw, rh = rect
    seen = set()
    for yy in range(y, y + rh, 3):
        for xx in range(x, x + rw, 3):
            seen.add(png_pixel(rgba, w, xx, yy, bpp))
            if len(seen) >= cap:
                return len(seen)
    return len(seen)


def main() -> int:
    if not os.path.exists(EXE):
        print(f"hawk.exe not found at {EXE}")
        return 2

    vx, vy, vw, vh = desktop()
    for stale in (LAYOUT,):
        if os.path.exists(stale):
            os.remove(stale)

    print("starting the Hawk OS window...")
    proc = subprocess.Popen([EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            stdin=subprocess.DEVNULL)

    # 1. the app starts and publishes its geometry
    layout: dict[str, tuple[int, int, int, int]] = {}
    deadline = time.time() + 20
    while time.time() < deadline:
        if proc.poll() is not None:
            break
        if os.path.exists(LAYOUT):
            try:
                layout = parse_layout(LAYOUT)
            except Exception:
                layout = {}
            if "window" in layout and "capture" in layout:
                break
        time.sleep(0.3)
    check("app starts and publishes its layout",
          "window" in layout and "capture" in layout and "client" in layout,
          f"{len(layout)} regions" if layout else f"no ui-layout.txt (exit {proc.poll()})")

    if not layout:
        proc.terminate()
        return 1

    win = layout["window"]
    check("window is a sensible size on this desktop",
          800 <= win[2] <= vw and 500 <= win[3] <= vh and vx <= win[0] and vy <= win[1],
          f"{win[2]}x{win[3]} at {win[0]},{win[1]} (desktop {vw}x{vh})")

    # 2. it is a real top-level window our own tooling can find
    wins = run("--windows").stdout
    found = [l for l in wins.splitlines() if "Hawk OS - desktop control agent" in l]
    check("app window is discoverable by title", bool(found),
          found[0].strip()[:80] if found else "not in --windows output")

    # 3. it actually rendered. Raise it first, then keep capturing until the
    #    window is composited on top - the same thing a user does by clicking it.
    run("--run", "focus Hawk OS - desktop control agent")
    time.sleep(0.6)
    path = os.path.join(SHOTS, "gui-verify.png")
    w = h = bpp = 0
    rgba = bytearray()
    accent = 0
    for attempt in range(10):
        run("--shot", path)
        w, h, rgba, meta = read_png(path)
        bpp = meta["bpp"]
        accent = count_colour(rgba, w, bpp, layout["run_goal"], (10, 132, 255))
        if accent >= 200:
            break
        time.sleep(0.5)

    check("accent button is drawn where it says it is", accent >= 200,
          f"{accent} accent pixels inside run_goal {layout['run_goal']}")

    idle_stop = count_colour(rgba, w, bpp, layout["stop"], (42, 42, 48))
    check("idle STOP button is visibly disabled", idle_stop >= 200,
          f"{idle_stop} disabled-state pixels inside stop {layout['stop']}")

    colours = distinct_colours(rgba, w, bpp, layout["preview"])
    check("live screen preview has picture content", colours >= 25,
          f"{colours} distinct colours in the preview region")

    # the header gradient is sampled inside the client area, not the OS caption
    client = layout.get("client", win)
    band = [png_pixel(rgba, w, client[0] + 700, client[1] + y, bpp) for y in (2, 20, 40, 58)]
    check("header uses a gradient rather than a flat fill", len(set(band)) >= 3,
          f"rows {band}")

    # 4. physically click the app's own Capture button with the app's own engine
    before = set(glob.glob(os.path.join(SHOTS, "*-app.png")))
    log_before = os.path.getsize(LOG) if os.path.exists(LOG) else 0
    run("--run", "focus Hawk OS - desktop control agent")
    time.sleep(0.4)
    cx = layout["capture"][0] + layout["capture"][2] // 2
    cy = layout["capture"][1] + layout["capture"][3] // 2
    click = run("--run", f"click left {cx} {cy}")
    new = set()
    deadline = time.time() + 4
    while time.time() < deadline:
        new = set(glob.glob(os.path.join(SHOTS, "*-app.png"))) - before
        if new:
            break
        time.sleep(0.4)
    if not new:
        # Windows occasionally swallows the first click on a freshly activated
        # window as the activation click. A person would simply click again,
        # so the test does too - the button still has to do its job.
        print("first click was swallowed by window activation; clicking again")
        click = run("--run", f"click left {cx} {cy}")
        deadline = time.time() + 8
        while time.time() < deadline:
            new = set(glob.glob(os.path.join(SHOTS, "*-app.png"))) - before
            if new:
                break
            time.sleep(0.4)

    check("a real click on Capture produced a real screenshot", bool(new),
          f"clicked {cx},{cy}; {os.path.basename(sorted(new)[0]) if new else 'no new shot'}")
    if new:
        # The file appears the instant the app opens it for writing, so give it
        # a moment to finish before decoding - reading a half-written PNG would
        # fail its CRC.
        target = sorted(new)[0]
        dims = None
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                w2, h2, _, _ = read_png(target)
                dims = (w2, h2)
                break
            except AssertionError:
                time.sleep(0.3)
        check("the button's screenshot is a valid PNG of the desktop", dims == (vw, vh),
              f"{dims[0]}x{dims[1]}" if dims else "never became a readable PNG")

    log_grew = (os.path.getsize(LOG) if os.path.exists(LOG) else 0) > log_before
    check("the app logged the click it received", log_grew,
          f"hawk.log grew by {(os.path.getsize(LOG) if os.path.exists(LOG) else 0) - log_before} bytes")
    check("the click command itself succeeded", click.returncode == 0, f"exit {click.returncode}")

    # 5. ask the window to close, the way a user would hit the X
    run("--run", "close Hawk OS")
    code = None
    try:
        code = proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.terminate()
        code = "still running"
    check("window closes cleanly when asked", code == 0, f"process exit: {code}")

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
