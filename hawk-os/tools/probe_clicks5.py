"""Click the app's Capture button 5 times; report which clicks registered."""
import glob, os, subprocess, sys, time, ctypes
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tests"))
from test_hawk import EXE, ROOT, SHOTS, run  # noqa: E402

user32 = ctypes.windll.user32
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


def fg():
    h = user32.GetForegroundWindow()
    b = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(h, b, 256)
    return b.value


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
time.sleep(0.6)
cap = layout["capture"]
cx, cy = cap[0] + cap[2] // 2, cap[1] + cap[3] // 2
seen = set(glob.glob(os.path.join(SHOTS, "*-app.png")))
for i in range(5):
    run("--run", f"click left {cx} {cy}")
    time.sleep(2.0)
    now = set(glob.glob(os.path.join(SHOTS, "*-app.png")))
    new = len(now - seen)
    seen = now
    print(f"click {i + 1}: fg={fg()!r} new_shots={new}")
run("--run", "close Hawk OS")
proc.wait(timeout=10)
