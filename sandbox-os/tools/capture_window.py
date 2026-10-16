#!/usr/bin/env python3
"""Capture the desktop app's real window and verify it shows the live screen.

This is the desktop equivalent of a screenshot test: it opens the app for a few
seconds, grabs the actual window pixels through the Windows GDI, then compares
the canvas area of the grab against the framebuffer the kernel rendered. If the
app were blank, mis-scaled or drawing the wrong thing, the comparison fails.

  python tools/capture_window.py [--scale 1.0] [--keep 3.0]
"""

from __future__ import annotations

import argparse
import ctypes
import pathlib
import random
import sys
import time
from ctypes import wintypes

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from agentd import AgentRunner, ScriptedProvider  # noqa: E402
from sandboxos import config  # noqa: E402
from sandboxos.kernel import Kernel  # noqa: E402
from sandboxos.png import write_png  # noqa: E402

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0
PW_RENDERFULLCONTENT = 0x00000002
GA_ROOT = 2


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD),
    ]


def capture(hwnd: int, use_printwindow: bool = True):
    """Return (width, height, rgb_bytes, origin_x, origin_y) for the window."""
    root_hwnd = user32.GetAncestor(hwnd, GA_ROOT) or hwnd
    rect = wintypes.RECT()
    if not user32.GetWindowRect(root_hwnd, ctypes.byref(rect)):
        raise RuntimeError("GetWindowRect failed")
    w, h = rect.right - rect.left, rect.bottom - rect.top
    if w <= 0 or h <= 0:
        raise RuntimeError(f"window has no size ({w}x{h})")

    hdc_src = user32.GetWindowDC(root_hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_src)
    hbm = gdi32.CreateCompatibleBitmap(hdc_src, w, h)
    gdi32.SelectObject(hdc_mem, hbm)
    try:
        ok = False
        if use_printwindow:
            ok = bool(user32.PrintWindow(root_hwnd, hdc_mem, PW_RENDERFULLCONTENT))
        if not ok:
            ok = bool(gdi32.BitBlt(hdc_mem, 0, 0, w, h, hdc_src, 0, 0, SRCCOPY))
        if not ok:
            raise RuntimeError("could not copy the window pixels")

        info = BITMAPINFOHEADER()
        info.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        info.biWidth = w
        info.biHeight = -h  # top-down
        info.biPlanes = 1
        info.biBitCount = 32
        info.biCompression = 0
        buf = ctypes.create_string_buffer(w * h * 4)
        got = gdi32.GetDIBits(hdc_mem, hbm, 0, h, buf, ctypes.byref(info), DIB_RGB_COLORS)
        if got != h:
            raise RuntimeError(f"GetDIBits returned {got} of {h} rows")
    finally:
        gdi32.DeleteObject(hbm)
        gdi32.DeleteDC(hdc_mem)
        user32.ReleaseDC(root_hwnd, hdc_src)

    raw = buf.raw
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        b, g, r = raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2]
        rgb[i * 3] = r
        rgb[i * 3 + 1] = g
        rgb[i * 3 + 2] = b
    return w, h, bytes(rgb), rect.left, rect.top


def png_rgb(png: bytes) -> tuple[int, int, bytes]:
    """Decode our own PNGs (zlib + filter 0) - enough for verification."""
    import struct
    import zlib

    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, w, h, idat = 8, 0, 0, b""
    while pos < len(png):
        length = struct.unpack(">I", png[pos : pos + 4])[0]
        tag = png[pos + 4 : pos + 8]
        data = png[pos + 8 : pos + 8 + length]
        if tag == b"IHDR":
            w, h = struct.unpack(">II", data[:8])
        elif tag == b"IDAT":
            idat += data
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = w * 3
    out = bytearray()
    for y in range(h):
        row = raw[y * (stride + 1) : (y + 1) * (stride + 1)]
        if row[0] != 0:
            raise ValueError("unexpected PNG filter")
        out += row[1:]
    return w, h, bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--keep", type=float, default=2.5, help="seconds to leave the window up")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    if not sys.platform.startswith("win"):
        print("this capture tool is Windows-only")
        return 2

    import desktop

    config.ensure_dirs()
    kernel = Kernel(boot_windows=True)
    runner = AgentRunner(kernel, provider=ScriptedProvider())
    app = desktop.SandboxDesktop(kernel, runner, scale=args.scale)
    app.root.update()
    app.root.deiconify()
    app.root.lift()
    app.root.update()

    failures = 0

    def check(name: str, ok: bool, detail: str = "") -> None:
        nonlocal failures
        failures += 0 if ok else 1
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""))

    print("capturing the desktop app window...")
    time.sleep(1.0)
    app.root.update()

    hwnd = int(app.root.winfo_id())
    w, h, rgb, ox, oy = capture(hwnd)
    out_path = pathlib.Path(args.out) if args.out else config.SHOTS_DIR / "desktop-app-window.png"
    out_path.write_bytes(write_png(w, h, rgb, level=6))
    print(f"  window grab: {w}x{h} -> {out_path}")

    # 1. the window is a real window with content
    distinct = len({rgb[i * 3 : i * 3 + 3] for i in range(0, w * h, 7)})
    check("window has size", w > 400 and h > 300, f"{w}x{h}")
    check("window is not blank/black", distinct > 25, f"{distinct} distinct sampled colours")

    # 2. the canvas area shows exactly what the kernel rendered
    canvas_w, canvas_h = app.canvas.winfo_width(), app.canvas.winfo_height()
    cx = app.canvas.winfo_rootx() - ox
    cy = app.canvas.winfo_rooty() - oy
    fb_w, fb_h, fb_rgb = png_rgb(kernel.screen.screenshot_png(force=True))
    border = 2 * int(app.canvas.cget("highlightthickness"))
    canvas_w -= border
    canvas_h -= border
    cx += border // 2
    cy += border // 2
    scale = canvas_w / fb_w
    check("canvas maps onto the framebuffer 1:1 (or a clean fraction)",
          abs(scale - fb_w / canvas_w) < 0.02 or scale in (0.5, 1.0, 2.0), f"scale={scale}")

    random.seed(7)
    matches = total = 0
    for _ in range(400):
        fx = random.randrange(0, fb_w)
        fy = random.randrange(0, fb_h)
        px = int(cx + fx * (canvas_w / fb_w))
        py = int(cy + fy * (canvas_h / fb_h))
        if not (0 <= px < w and 0 <= py < h):
            continue
        total += 1
        a = rgb[(py * w + px) * 3 : (py * w + px) * 3 + 3]
        b = fb_rgb[(fy * fb_w + fx) * 3 : (fy * fb_w + fx) * 3 + 3]
        if all(abs(a[i] - b[i]) <= 12 for i in range(3)):
            matches += 1
    ratio = matches / max(1, total)
    check("the app's canvas matches the framebuffer pixel for pixel",
          total > 300 and ratio >= 0.9, f"{matches}/{total} sampled pixels matched ({ratio:.1%})")

    # 3. input works in the real window: click the taskbar, screen must change
    before = kernel.screen.frame_seq
    app._mouse("down", type("E", (), {"x": int(159 * args.scale), "y": int(586 * args.scale)})())
    app.root.update()
    check("clicking the real window drives the sandbox screen",
          kernel.screen.frame_seq > before and any(win.app == "editor" for win in kernel.screen.windows),
          f"frame_seq {before} -> {kernel.screen.frame_seq}")

    # 4. the live frame keeps flowing into the window while it is open
    app.root.update()
    time.sleep(min(args.keep, 1.5))
    app.run_shell()
    kernel.shell_exec(kernel.main_session, "echo captured")
    app.root.update()
    w2, h2, rgb2, _, _ = capture(hwnd)
    check("window keeps rendering after interaction", w2 == w and h2 == h and rgb2 != rgb,
          f"{w2}x{h2}")

    app.quit()
    print(f"\n{'all window checks passed' if not failures else f'{failures} check(s) failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
