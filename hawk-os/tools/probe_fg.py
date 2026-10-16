"""Diagnose whether the injected click is swallowed by window activation."""
import glob, os, subprocess, sys, time, ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tests"))
from test_hawk import EXE, ROOT, SHOTS, run  # noqa: E402

LAYOUT = os.path.join(ROOT, "ui-layout.txt")


def parse_layout(path):
    out = {}
    for line in open(path, encoding="utf-8"):
        if line.startswith("#"):
            continue
        p = line.split()
        if len(p) == 5 and all(q.lstrip("-").isdigit() for q in p[1:]):
            out[p[0]] = tuple(int(q) for q in p[1:])
    return out


def fg_title():
    hwnd = user32.GetForegroundWindow()
    buf = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, buf, 256)
    return buf.value


if os.path.exists(LAYOUT):
    os.remove(LAYOUT)
proc = subprocess.Popen([EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
deadline = time.time() + 20
layout = {}
while time.time() < deadline:
    if os.path.exists(LAYOUT):
        try:
            layout = parse_layout(LAYOUT)
        except Exception:
            layout = {}
        if "capture" in layout:
            break
    time.sleep(0.3)

run("--run", "focus Hawk OS - desktop control agent")
time.sleep(0.5)
print("foreground after focus:", repr(fg_title()))
cap = layout["capture"]
cx, cy = cap[0] + cap[2] // 2, cap[1] + cap[3] // 2
before = set(glob.glob(os.path.join(SHOTS, "*-app.png")))
run("--run", f"click left {cx} {cy}")
time.sleep(1.5)
new = set(glob.glob(os.path.join(SHOTS, "*-app.png"))) - before
print("after 1st click, new shot:", bool(new), "| fg:", repr(fg_title()))
# a second click, now that the window is definitely foreground
run("--run", f"click left {cx} {cy}")
deadline = time.time() + 6
while time.time() < deadline:
    new = set(glob.glob(os.path.join(SHOTS, "*-app.png"))) - before
    if new:
        break
    time.sleep(0.4)
print("after 2nd click, new shot:", bool(new), [os.path.basename(p) for p in new])
run("--run", "close Hawk OS")
proc.wait(timeout=10)
