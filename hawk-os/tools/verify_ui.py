#!/usr/bin/env python3
"""Source-level regression checks for the Hawk desktop control-center UI."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
ui = (root / "src" / "ui.cpp").read_text(encoding="utf-8")
main = (root / "src" / "main.cpp").read_text(encoding="utf-8")

checks = [
    ("no screenshot preview renderer", "StretchDIBits" not in ui and "compute_preview_rect" not in ui),
    ("no cached preview image in UI state", "Image preview;" not in ui and "g_ui->preview" not in ui),
    ("no periodic screenshot capture", not re.search(r"case WM_TIMER\s*:\s*[^}]*capture_screen\s*\(", ui)),
    ("manual capture still saves a screenshot", 'take_shot("app")' in ui),
    ("native Fluent icon font used", "Segoe MDL2 Assets" in ui and "g_ui->f_icon" in ui),
    ("run status animation invalidates while active", 'g_ui->run_state == "running" || g_ui->run_state == "stopping"' in ui),
    ("NVIDIA dotenv loader exists", "static void read_dotenv(Config* cfg)" in main and 'k == "NVIDIA_API_KEY"' in main),
]

failed = False
for label, ok in checks:
    print(f"[{'PASS' if ok else 'FAIL'}] {label}")
    failed |= not ok

sys.exit(1 if failed else 0)
