"""One-off diagnostic for the Capture-button click (not shipped as a test)."""
import glob, os, subprocess, sys, time
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
print("layout:", {k: layout[k] for k in ("window", "capture", "run_goal") if k in layout})

run("--run", "focus Hawk OS - desktop control agent")
time.sleep(0.5)
cap = layout["capture"]
cx, cy = cap[0] + cap[2] // 2, cap[1] + cap[3] // 2
print("pixel at button:", run("--pixel", str(cx), str(cy)).stdout.strip())
before = set(glob.glob(os.path.join(SHOTS, "*-app.png")))
r = run("--run", f"click left {cx} {cy}")
print("click rc:", r.returncode, "|", [l for l in r.stdout.splitlines() if "click" in l])
deadline = time.time() + 8
new = set()
while time.time() < deadline:
    new = set(glob.glob(os.path.join(SHOTS, "*-app.png"))) - before
    if new:
        break
    time.sleep(0.4)
print("new shots:", [os.path.basename(p) for p in new] or "NONE")
print("windows at that point:")
print(run("--run", f"pixel {cx} {cy}").stdout.strip())
run("--run", "close Hawk OS")
proc.wait(timeout=10)
