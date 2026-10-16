"""The virtual screen of SandboxOS: a macOS-style desktop.

A framebuffer, a window manager that looks and behaves like a modern desktop -
menu bar, Dock with running indicators, rounded windows with traffic lights,
soft wallpaper, menus that actually do things - and the sandbox's apps:
Terminal, Files, Editor, Activity, Agent and About.

Nothing here touches the host display. The screen is a plain RGB buffer; the
desktop app and the browser console show it, and the AI agent perceives it
through `screen_describe` and acts through synthetic mouse and keyboard events.
"""

from __future__ import annotations

import math
import posixpath
import time

from . import font
from .png import write_png

# --- macOS-flavoured dark palette -----------------------------------------
WALL_TOP = (40, 48, 96)
WALL_BOTTOM = (12, 13, 22)
WALL_GLOW = (86, 104, 200)

MENUBAR_BG = (30, 30, 34)
MENUBAR_TEXT = (236, 236, 242)
MENUBAR_DIM = (168, 168, 176)

WIN_BG = (44, 44, 48)
CONTENT_BG = (30, 30, 34)
TITLEBAR_ACTIVE = (52, 52, 57)
TITLEBAR_IDLE = (42, 42, 46)
SHEET_BG = (58, 58, 64)
SEP = (76, 76, 82)

TEXT = (240, 240, 245)
MUTED = (152, 152, 160)
DIM = (110, 110, 118)
ACCENT = (10, 132, 255)
ACCENT_DARK = (0, 92, 190)
PURPLE = (175, 82, 222)
GREEN = (39, 201, 63)
YELLOW = (255, 189, 46)
RED = (255, 95, 87)
OK = (48, 209, 88)
WARN = (255, 214, 10)
ERR = (255, 105, 97)
FOLDER = (94, 168, 255)
DOC = (196, 196, 204)

TERM_BG = (14, 14, 16)
TERM_TEXT = (226, 226, 232)
TERM_PROMPT = (110, 220, 130)

DOCK_BG = (48, 48, 54)
DOCK_BORDER = (92, 92, 100)
DOCK_LABEL = (238, 238, 244)

MENUBAR_H = 24
DOCK_H = 68
DOCK_MARGIN = 6
DOCK_ICON = 40

CURSOR_SPRITE = (
    "X....",
    "XX...",
    "XXX..",
    "XXXX.",
    "XXXXX",
    "XX...",
    "X.X..",
)

_wallpaper_cache: dict[tuple[int, int], bytes] = {}


def make_wallpaper(width: int, height: int, cols: int = 96, rows: int = 40) -> bytes:
    """Smooth gradient + soft glow, built on a coarse grid and repeated.

    Cheap and pleasant: the grid steps are under half a colour level, so the
    result reads as a smooth wallpaper instead of banding.
    """
    key = (width, height)
    if key in _wallpaper_cache:
        return _wallpaper_cache[key]

    block_w = max(1, math.ceil(width / cols))
    block_h = max(1, math.ceil(height / rows))
    cx, cy, rad = width * 0.5, height * 0.20, max(width, height) * 0.62

    row_templates: list[bytes] = []
    for gy in range(rows):
        y = min(height - 1, gy * block_h + block_h // 2)
        t = y / max(1, height - 1)
        base = [WALL_TOP[i] + (WALL_BOTTOM[i] - WALL_TOP[i]) * t for i in range(3)]
        row = bytearray()
        for gx in range(cols):
            x = min(width - 1, gx * block_w + block_w // 2)
            dx = (x - cx) / max(1, width)
            dy = (y - cy) / max(1, height)
            d = math.hypot(dx, dy * 1.25)
            glow = max(0.0, 1.0 - d / 0.62) ** 2 * 0.55
            for c in range(3):
                v = base[c] + glow * WALL_GLOW[c]
                row.append(int(max(0, min(255, v))))
        row_templates.append(bytes(row) * block_w)

    buf = bytearray()
    for gy in range(rows):
        buf += row_templates[gy] * block_h
    data = bytes(buf[: width * height * 3])
    _wallpaper_cache[key] = data
    return data


class Framebuffer:
    def __init__(self, width: int, height: int, bg: tuple = (0, 0, 0)):
        self.width = width
        self.height = height
        self.buf = bytearray(width * height * 3)
        self.fill(bg)

    # -- primitives --------------------------------------------------------
    def fill(self, color) -> None:
        row = bytes(color) * self.width
        for y in range(self.height):
            self.buf[y * self.width * 3 : (y + 1) * self.width * 3] = row

    def blit(self, x: int, y: int, width: int, height: int, data: bytes) -> None:
        """Copy a raw RGB block (used for the wallpaper and cached panels)."""
        for row in range(height):
            dy = y + row
            if not (0 <= dy < self.height):
                continue
            src = row * width * 3
            x0 = max(0, x)
            x1 = min(self.width, x + width)
            if x1 <= x0:
                continue
            dst0 = (dy * self.width + x0) * 3
            dst1 = (dy * self.width + x1) * 3
            self.buf[dst0:dst1] = data[src + (x0 - x) * 3 : src + (x1 - x) * 3]

    def pixel(self, x: int, y: int, color) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            i = (y * self.width + x) * 3
            self.buf[i] = color[0]
            self.buf[i + 1] = color[1]
            self.buf[i + 2] = color[2]

    def get_pixel(self, x: int, y: int):
        if not (0 <= x < self.width and 0 <= y < self.height):
            return None
        i = (y * self.width + x) * 3
        return (self.buf[i], self.buf[i + 1], self.buf[i + 2])

    def fill_rect(self, x: int, y: int, w: int, h: int, color) -> None:
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(self.width, x + w), min(self.height, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        row = bytes(color) * (x1 - x0)
        for yy in range(y0, y1):
            self.buf[(yy * self.width + x0) * 3 : (yy * self.width + x1) * 3] = row

    def blend_rect(self, x: int, y: int, w: int, h: int, color, alpha: float) -> None:
        """Translucent panel, used for the Dock and menu dropdowns."""
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(self.width, x + w), min(self.height, y + h)
        if x1 <= x0 or y1 <= y0 or alpha <= 0:
            return
        a = max(0.0, min(1.0, alpha))
        inv = 1.0 - a
        for yy in range(y0, y1):
            base = yy * self.width * 3
            for xx in range(x0, x1):
                i = base + xx * 3
                self.buf[i] = int(self.buf[i] * inv + color[0] * a)
                self.buf[i + 1] = int(self.buf[i + 1] * inv + color[1] * a)
                self.buf[i + 2] = int(self.buf[i + 2] * inv + color[2] * a)

    def rect(self, x: int, y: int, w: int, h: int, color) -> None:
        self.fill_rect(x, y, w, 1, color)
        self.fill_rect(x, y + h - 1, w, 1, color)
        self.fill_rect(x, y, 1, h, color)
        self.fill_rect(x + w - 1, y, 1, h, color)

    def fill_circle(self, cx: int, cy: int, r: int, color) -> None:
        for dy in range(-r, r + 1):
            span = int(math.sqrt(max(0, r * r - dy * dy)))
            self.fill_rect(cx - span, cy + dy, span * 2 + 1, 1, color)

    def circle(self, cx: int, cy: int, r: int, color) -> None:
        for dy in range(-r, r + 1):
            span = int(math.sqrt(max(0, r * r - dy * dy)))
            self.pixel(cx - span, cy + dy, color)
            self.pixel(cx + span, cy + dy, color)
        for dx in range(-r, r + 1):
            span = int(math.sqrt(max(0, r * r - dx * dx)))
            self.pixel(cx + dx, cy - span, color)
            self.pixel(cx + dx, cy + span, color)

    def rounded_rect(self, x: int, y: int, w: int, h: int, r: int, color) -> None:
        r = max(0, min(r, min(w, h) // 2))
        for row in range(h):
            if row < r:
                inset = r - int(math.sqrt(max(0, r * r - (r - 1 - row) ** 2)))
            elif row >= h - r:
                inset = r - int(math.sqrt(max(0, r * r - (row - (h - r)) ** 2)))
            else:
                inset = 0
            self.fill_rect(x + inset, y + row, w - inset * 2, 1, color)

    def rounded_panel(self, x: int, y: int, w: int, h: int, r: int, fill, border=None,
                      alpha: float | None = None) -> None:
        if alpha is None:
            self.rounded_rect(x, y, w, h, r, fill)
        else:
            # approximating translucency with a blend per row keeps it cheap
            for row in range(h):
                if row < r:
                    inset = r - int(math.sqrt(max(0, r * r - (r - 1 - row) ** 2)))
                elif row >= h - r:
                    inset = r - int(math.sqrt(max(0, r * r - (row - (h - r)) ** 2)))
                else:
                    inset = 0
                self.blend_rect(x + inset, y + row, w - inset * 2, 1, fill, alpha)
        if border:
            for row in range(h):
                if row < r:
                    inset = r - int(math.sqrt(max(0, r * r - (r - 1 - row) ** 2)))
                elif row >= h - r:
                    inset = r - int(math.sqrt(max(0, r * r - (row - (h - r)) ** 2)))
                else:
                    inset = 0
                if row in (0, h - 1):
                    self.fill_rect(x + inset, y + row, w - inset * 2, 1, border)
                else:
                    self.pixel(x + inset, y + row, border)
                    self.pixel(x + w - 1 - inset, y + row, border)

    def text(self, x: int, y: int, s: str, color=TEXT, scale: int = 1) -> int:
        cx = x
        for ch in s:
            rows = font.glyph(ch)
            for ry, rowbits in enumerate(rows):
                for rx, bit in enumerate(rowbits):
                    if bit == "#":
                        if scale == 1:
                            self.pixel(cx + rx, y + ry, color)
                        else:
                            self.fill_rect(cx + rx * scale, y + ry * scale, scale, scale, color)
            cx += font.CELL_W * scale
        return cx - x

    def text_centered(self, x: int, y: int, w: int, s: str, color=TEXT, scale: int = 1) -> int:
        return self.text(x + max(0, (w - font.text_width(s, scale)) // 2), y, s, color, scale)

    def text_right(self, x: int, y: int, w: int, s: str, color=TEXT, scale: int = 1) -> int:
        return self.text(x + max(0, w - font.text_width(s, scale)), y, s, color, scale)

    def shadow(self, x: int, y: int, w: int, h: int, r: int = 10) -> None:
        for depth, alpha in ((5, 0.16), (3, 0.14), (1, 0.12)):
            self.rounded_panel(x - depth, y - depth + 2, w + depth * 2, h + depth * 2, r + depth,
                               (0, 0, 0), alpha=alpha)

    def to_png(self, level: int = 5) -> bytes:
        return write_png(self.width, self.height, bytes(self.buf), level)


# --------------------------------------------------------------------------
# windows
# --------------------------------------------------------------------------
class Window:
    app = "window"
    min_w, min_h = 260, 150

    def __init__(self, wm, wid: int, title: str, x: int, y: int, w: int, h: int):
        self.wm = wm
        self.id = wid
        self.title = title
        self.x, self.y = int(x), int(y)
        self.w, self.h = max(self.min_w, int(w)), max(self.min_h, int(h))
        self.scroll = 0
        self.created_at = time.time()
        self.minimized = False
        self.zoomed = False
        self._restore_rect: tuple[int, int, int, int] | None = None

    # -- geometry ----------------------------------------------------------
    @property
    def title_h(self) -> int:
        return 26

    @property
    def radius(self) -> int:
        return 9

    @property
    def content_rect(self) -> tuple[int, int, int, int]:
        return (self.x + 1, self.y + self.title_h, self.w - 2, self.h - self.title_h - 1)

    def traffic_lights(self) -> list[tuple[str, tuple[int, int, int, int]]]:
        """red/yellow/green dots, macOS order and colours."""
        y = self.y + 9
        x = self.x + 12
        out = []
        for name, color in (("close", RED), ("minimize", YELLOW), ("zoom", GREEN)):
            out.append((name, (x, y, 10, 10)))
            x += 20
        return out

    def close_button_rect(self) -> tuple[int, int, int, int]:
        return self.traffic_lights()[0][1]

    def contains(self, px: int, py: int) -> bool:
        return self.x <= px < self.x + self.w and self.y <= py < self.y + self.h

    def in_title(self, px: int, py: int) -> bool:
        return self.contains(px, py) and py < self.y + self.title_h

    def to_local(self, px: int, py: int) -> tuple[int, int]:
        cx, cy, _, _ = self.content_rect
        return px - cx, py - cy

    # -- rendering ---------------------------------------------------------
    def draw(self, fb: Framebuffer, focused: bool) -> None:
        if self.minimized:
            return
        fb.shadow(self.x, self.y, self.w, self.h, self.radius)
        fb.rounded_rect(self.x, self.y, self.w, self.h, self.radius, WIN_BG)
        # title bar with rounded top corners, then squared off at the bottom
        fb.rounded_rect(self.x, self.y, self.w, self.title_h + self.radius,
                        self.radius, TITLEBAR_ACTIVE if focused else TITLEBAR_IDLE)
        fb.fill_rect(self.x, self.y + self.title_h - 1, self.w, self.radius + 1, WIN_BG)
        fb.fill_rect(self.x, self.y + self.title_h - 1, self.w, 1, SEP if focused else TITLEBAR_IDLE)
        for name, (lx, ly, lw, lh) in self.traffic_lights():
            fb.fill_circle(lx + lw // 2, ly + lh // 2, lw // 2,
                           {"close": RED, "minimize": YELLOW, "zoom": GREEN}[name])
        title = self.title[: max(4, (self.w - 110) // font.CELL_W)]
        fb.text_centered(self.x, self.y + 10, self.w, title,
                         TEXT if focused else MUTED)
        self.draw_content(fb)
        # hairline border
        fb.rounded_panel(self.x, self.y, self.w, self.h, self.radius, (0, 0, 0), border=(84, 84, 92))

    def draw_content(self, fb: Framebuffer) -> None:
        raise NotImplementedError

    def draw_scrollbar(self, fb: Framebuffer, total: int, visible: int) -> None:
        cx, cy, cw, ch = self.content_rect
        if total <= visible:
            return
        track_x = cx + cw - 8
        fb.fill_rect(track_x, cy + 2, 6, ch - 4, (26, 26, 30))
        frac = min(1.0, visible / total)
        bar_h = max(18, int((ch - 4) * frac))
        pos = int((ch - 4 - bar_h) * (self.scroll / max(1, total - visible)))
        fb.rounded_rect(track_x, cy + 2 + pos, 6, bar_h, 3, (120, 120, 128))

    # -- events ------------------------------------------------------------
    def on_key(self, name: str, text: str | None) -> bool:
        return False

    def on_mouse(self, kind: str, lx: int, ly: int, button: str, delta: int) -> bool:
        if kind == "wheel":
            self.scroll = max(0, self.scroll + (-1 if delta < 0 else 1) * 3)
            return True
        return False

    def on_click(self, lx: int, ly: int, button: str) -> bool:
        return False

    # -- introspection -----------------------------------------------------
    def describe(self) -> list[str]:
        return []

    def serialize(self) -> dict:
        return {
            "id": self.id,
            "app": self.app,
            "title": self.title,
            "rect": [self.x, self.y, self.w, self.h],
            "minimized": self.minimized,
            "content": self.describe(),
        }


class TerminalWindow(Window):
    app = "terminal"
    min_w, min_h = 360, 180

    def __init__(self, wm, wid, session, title=None, x=70, y=80, w=610, h=330):
        super().__init__(wm, wid, title or f"{session.user} - {session.host} - sssh", x, y, w, h)
        self.session = session
        self.lines: list[tuple[str, tuple]] = [
            (wm.kernel.banner().split("\n")[0], MUTED),
            ("Type 'help' for the built-in command set.", MUTED),
            ("", TEXT),
        ]
        self.input = ""
        self.hist_idx: int | None = None
        self.auto_scroll = True

    @property
    def cols(self) -> int:
        return max(20, (self.w - 26) // font.CELL_W)

    def rows_available(self) -> int:
        return max(4, (self.h - self.title_h - 14) // font.CELL_H)

    def _print(self, text: str, color=TEXT) -> None:
        for line in text.split("\n") if text else [""]:
            for wrapped in font.wrap(line, self.cols) or [""]:
                self.lines.append((wrapped, color))
        if len(self.lines) > 4000:
            self.lines = self.lines[-2000:]
        self.auto_scroll = True

    def run(self, command: str) -> None:
        self._print(self.session.prompt() + command, TERM_PROMPT)
        self.wm.kernel.audit.record(
            "shell_exec", actor="terminal", shell=f"{self.session.user}@{self.session.host}",
            command=command, cwd=self.session.cwd,
        )
        if command.strip() == "clear":
            self.lines = []
            return
        result = self.wm.kernel.shell_exec(self.session, command)
        if result.text:
            self._print(result.text, TERM_TEXT if result.code == 0 else ERR)

    def on_key(self, name: str, text: str | None) -> bool:
        if text:
            self.input += text
            return True
        if name == "Enter":
            cmd = self.input
            self.input = ""
            self.hist_idx = None
            self.run(cmd)
            return True
        if name == "Backspace":
            self.input = self.input[:-1]
            return True
        if name == "Up":
            hist = self.session.history
            if hist:
                self.hist_idx = len(hist) - 1 if self.hist_idx is None else max(0, self.hist_idx - 1)
                self.input = hist[self.hist_idx]
            return True
        if name == "Down":
            hist = self.session.history
            if hist and self.hist_idx is not None:
                if self.hist_idx >= len(hist) - 1:
                    self.hist_idx = None
                    self.input = ""
                else:
                    self.hist_idx += 1
                    self.input = hist[self.hist_idx]
            return True
        if name in ("PageUp", "PageDown"):
            page = self.rows_available() - 1
            self.auto_scroll = name == "End" or self.auto_scroll
            if name == "PageUp":
                self.auto_scroll = False
                self.scroll = max(0, self.scroll - page)
            else:
                self.auto_scroll = False
                self.scroll = min(max(0, len(self.lines) - self.rows_available()), self.scroll + page)
            return True
        if name == "End":
            self.auto_scroll = True
            return True
        return False

    def on_mouse(self, kind, lx, ly, button, delta) -> bool:
        if kind == "wheel":
            self.auto_scroll = False
            self.scroll = max(0, self.scroll + (-1 if delta < 0 else 1) * 3)
            self.scroll = min(self.scroll, max(0, len(self.lines) - self.rows_available()))
            return True
        return False

    def draw_content(self, fb: Framebuffer) -> None:
        cx, cy, cw, ch = self.content_rect
        fb.fill_rect(cx, cy + 1, cw, ch - 1, TERM_BG)
        rows = self.rows_available()
        lines = self.lines
        if self.auto_scroll:
            self.scroll = max(0, len(lines) + 1 - rows)
        start = min(self.scroll, max(0, len(lines) + 1 - rows)) if len(lines) + 1 > rows else 0
        y = cy + 5
        visible = lines[start : start + rows]
        for text, color in visible:
            fb.text(cx + 10, y, text[: self.cols], color)
            y += font.CELL_H
        if y <= cy + ch - font.CELL_H and len(lines) - start <= rows:
            prompt = self.session.prompt()
            cursor_x = fb.text(cx + 10, y, (prompt + self.input)[: self.cols], TERM_PROMPT)
            fb.fill_rect(cursor_x + 1, y, 5, 8, TERM_TEXT)
        self.draw_scrollbar(fb, len(lines), rows)

    def describe(self) -> list[str]:
        rows = self.rows_available()
        shown = self.lines[max(0, len(self.lines) - rows) :]
        out = [f"terminal: {self.session.prompt()}{self.input}"]
        out += [f"  {t}" for t, _ in shown if t]
        return out


class FilesWindow(Window):
    """Finder: browse, create files and folders, open things."""

    app = "files"
    min_w, min_h = 320, 200

    def __init__(self, wm, wid, path="/", x=700, y=80, w=250, h=330):
        super().__init__(wm, wid, "Files", x, y, w, h)
        self.path = path
        self.entries: list[dict] = []
        self.sel = 0
        self.status = ""
        self.refresh()

    # -- model -------------------------------------------------------------
    def refresh(self) -> None:
        try:
            self.entries = self.wm.kernel.vfs.list_dir(self.path)
            self.status = ""
        except Exception as exc:
            self.entries = []
            self.status = str(exc)
            self.wm.kernel.notify(f"files: {exc}", level="error")
        self.entries.sort(key=lambda e: (e["type"] == "file", e["name"].lower()))
        self.sel = max(0, min(self.sel, len(self.entries) - 1))
        self.title = f"Files - {self.path}"

    def rows_available(self) -> int:
        return max(2, (self.h - self.title_h - 34) // font.CELL_H)

    def _unique_name(self, base: str, ext: str = "") -> str:
        existing = {e["name"] for e in self.entries}
        n = 0
        while True:
            n += 1
            candidate = f"{base}{'' if n == 1 else f'-{n}'}{ext}"
            if candidate not in existing and not self.wm.kernel.vfs.exists(
                    posixpath.join(self.path, candidate)):
                return candidate

    def new_file(self, name: str | None = None) -> str | None:
        """Create an empty file in this folder and open it for editing."""
        target_dir = self.path if self.entries or self.path == "/" else self.path
        if not self.wm.kernel.vfs.is_dir(target_dir):
            return None
        fname = name or self._unique_name("untitled", ".txt")
        full = posixpath.join(target_dir, fname)
        try:
            self.wm.kernel.vfs.write_text(full, "")
        except Exception as exc:
            self.status = str(exc)
            return None
        self.wm.kernel.audit.record("file_create", actor="files-app", path=full, kind="file")
        self.wm.kernel.emit("fs", f"created {full}")
        self.refresh()
        for i, entry in enumerate(self.entries):
            if entry["name"] == fname:
                self.sel = i
                break
        self.wm.kernel.open_editor(full)
        return full

    def new_folder(self, name: str | None = None) -> str | None:
        fname = name or self._unique_name("untitled folder")
        full = posixpath.join(self.path, fname)
        try:
            self.wm.kernel.vfs.mkdir(full)
        except Exception as exc:
            self.status = str(exc)
            return None
        self.wm.kernel.audit.record("file_create", actor="files-app", path=full, kind="dir")
        self.wm.kernel.emit("fs", f"created {full}/")
        self.refresh()
        for i, entry in enumerate(self.entries):
            if entry["name"] == fname:
                self.sel = i
                break
        return full

    def open_selected(self) -> None:
        if not self.entries:
            return
        entry = self.entries[self.sel]
        if entry["type"] == "dir":
            self.path = entry["path"]
            self.sel = 0
            self.refresh()
        else:
            self.wm.kernel.open_editor(entry["path"])
            self.wm.kernel.audit.record("open_file", actor="files-app", path=entry["path"])

    def go_up(self) -> None:
        if self.path != "/":
            self.path = posixpath.dirname(self.path.rstrip("/")) or "/"
            self.sel = 0
            self.refresh()

    # -- layout ------------------------------------------------------------
    def toolbar_rects(self) -> dict:
        cx, cy, cw, _ = self.content_rect
        return {
            "file": (cx + 8, cy + 6, 58, 16),
            "folder": (cx + 70, cy + 6, 68, 16),
            "up": (cx + 142, cy + 6, 34, 16),
            "home": (cx + 180, cy + 6, 42, 16),
        }

    def draw_content(self, fb: Framebuffer) -> None:
        cx, cy, cw, ch = self.content_rect
        fb.fill_rect(cx, cy + 1, cw, ch - 1, CONTENT_BG)
        for label, key, (bx, by, bw, bh) in [
            ("+ file", "file", self.toolbar_rects()["file"]),
            ("+ folder", "folder", self.toolbar_rects()["folder"]),
            ("up", "up", self.toolbar_rects()["up"]),
            ("home", "home", self.toolbar_rects()["home"]),
        ]:
            fb.rounded_rect(bx, by, bw, bh, 4, (58, 58, 64))
            fb.rounded_panel(bx, by, bw, bh, 4, (0, 0, 0), border=(96, 96, 104))
            fb.text_centered(bx, by + 4, bw, label, TEXT)
        y0 = cy + 28
        rows = self.rows_available()
        start = min(self.scroll, max(0, len(self.entries) - rows))
        for i, entry in enumerate(self.entries[start : start + rows]):
            idx = start + i
            y = y0 + i * font.CELL_H
            if idx == self.sel:
                fb.rounded_rect(cx + 4, y - 1, cw - 12, font.CELL_H + 1, 3, ACCENT)
            colour = TEXT if idx == self.sel else (FOLDER if entry["type"] == "dir" else DOC)
            icon = "[folder]" if entry["type"] == "dir" else "[doc]"
            fb.text(cx + 8, y, icon, colour)
            name_colour = TEXT if idx == self.sel else TEXT
            fb.text(cx + 58, y, entry["name"][: (cw - 110) // font.CELL_W], name_colour)
            if entry["type"] == "file":
                size = f"{entry['size']}B"
                fb.text_right(cx + 8, y, cw - 22, size, MUTED)
        if not self.entries:
            fb.text(cx + 10, cy + 36, "This folder is empty", MUTED)
            fb.text(cx + 10, cy + 48, "Use '+ file' to create one", DIM)
        if self.status:
            fb.text(cx + 8, cy + ch - 12, self.status[: (cw - 16) // font.CELL_W], ERR)
        self.draw_scrollbar(fb, len(self.entries), rows)

    # -- events ------------------------------------------------------------
    def on_key(self, name, text) -> bool:
        if name == "Up":
            self.sel = max(0, self.sel - 1)
            return True
        if name == "Down":
            self.sel = min(max(0, len(self.entries) - 1), self.sel + 1)
            return True
        if name in ("Enter", "Right"):
            self.open_selected()
            return True
        if name in ("Backspace", "Left"):
            self.go_up()
            return True
        if name == "Home":
            self.path = "/"
            self.sel = 0
            self.refresh()
            return True
        if name in ("Ctrl+N", "n") and name == "Ctrl+N":
            self.new_file()
            return True
        return False

    def on_click(self, lx, ly, button) -> bool:
        cx, cy, _, _ = self.content_rect
        for label, (bx, by, bw, bh) in self.toolbar_rects().items():
            if bx - cx <= lx < bx - cx + bw and by - cy <= ly < by - cy + bh:
                if label == "file":
                    self.new_file()
                elif label == "folder":
                    self.new_folder()
                elif label == "up":
                    self.go_up()
                else:
                    self.path = "/"
                    self.sel = 0
                    self.refresh()
                return True
        row = (ly - 28) // font.CELL_H
        if row >= 0:
            idx = self.scroll + row
            if 0 <= idx < len(self.entries):
                if idx == self.sel:
                    self.open_selected()
                else:
                    self.sel = idx
                return True
        return False

    def on_mouse(self, kind, lx, ly, button, delta) -> bool:
        if kind == "wheel":
            self.scroll = max(0, min(self.scroll + (-1 if delta < 0 else 1) * 2,
                                     max(0, len(self.entries) - self.rows_available())))
            return True
        return False

    def describe(self) -> list[str]:
        rows = self.rows_available()
        start = min(self.scroll, max(0, len(self.entries) - rows))
        selected = self.entries[self.sel]["name"] if self.entries else "-"
        out = [f"files: {self.path}  (selected: {selected})",
               f"  buttons: + file@(+8,+6) + folder@(+70,+6) up@(+142,+6) home@(+180,+6) "
               f"relative to window content"]
        for entry in self.entries[start : start + rows]:
            mark = ">" if self.entries.index(entry) == self.sel else " "
            out.append(f" {mark} {entry['type'][:3]} {entry['name']} {entry['size']}")
        return out


# --- part 2 below ---
