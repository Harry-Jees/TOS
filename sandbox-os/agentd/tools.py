"""The agent's hands.

Every tool ultimately calls the SandboxOS kernel, so the same jail, quotas and
audit log that protect the host from the shell protect it from the agent too.
Tool results are always text (JSON or plain) so any model can read them.
"""

from __future__ import annotations

import json
import time

from sandboxos import config
from sandboxos.vfs import VfsError

MAX_RESULT = config.AGENT_MAX_TOOL_RESULT


def _schema(name: str, description: str, properties: dict | None = None, required: list[str] | None = None):
    return {
        "type": "function",
        "function": {
            "name": name,
            "description": description,
            "parameters": {
                "type": "object",
                "properties": properties or {},
                "required": required or [],
            },
        },
    }


TOOL_SCHEMAS = [
    _schema("sys_info", "Machine state: uptime, windows, disk usage, agent state.", {}),
    _schema("fs_list", "List a directory.", {"path": {"type": "string", "description": "e.g. /home/user"}}),
    _schema("fs_read", "Read a text file.", {"path": {"type": "string"}}, ["path"]),
    _schema(
        "fs_write",
        "Create or overwrite a text file (parents are created automatically).",
        {"path": {"type": "string"}, "content": {"type": "string"},
         "append": {"type": "boolean", "description": "append instead of overwrite"}},
        ["path", "content"],
    ),
    _schema("fs_mkdir", "Create a directory.", {"path": {"type": "string"}}, ["path"]),
    _schema(
        "fs_delete",
        "Delete a file or directory.",
        {"path": {"type": "string"}, "recursive": {"type": "boolean"}},
        ["path"],
    ),
    _schema("fs_move", "Move/rename a file or directory.", {"src": {"type": "string"}, "dst": {"type": "string"}}, ["src", "dst"]),
    _schema("fs_stat", "Metadata for a path.", {"path": {"type": "string"}}, ["path"]),
    _schema(
        "fs_search",
        "Regex search across file contents.",
        {"pattern": {"type": "string"}, "path": {"type": "string"}},
        ["pattern"],
    ),
    _schema(
        "shell_run",
        "Run a command in the sandbox shell and get its output. Supports pipes, "
        "redirection and ; && ||. No host binaries exist.",
        {"command": {"type": "string"}},
        ["command"],
    ),
    _schema("screen_describe", "Read the virtual screen as text: windows, rects, focus, visible content, cursor.", {}),
    _schema(
        "screen_screenshot",
        "Save a PNG of the current screen for the human to inspect.",
        {"label": {"type": "string"}},
    ),
    _schema("screen_move", "Move the mouse cursor.", {"x": {"type": "integer"}, "y": {"type": "integer"}}, ["x", "y"]),
    _schema(
        "screen_click",
        "Click the mouse at a screen position.",
        {"x": {"type": "integer"}, "y": {"type": "integer"},
         "button": {"type": "string", "enum": ["left", "right"]},
         "double": {"type": "boolean"}},
        ["x", "y"],
    ),
    _schema(
        "screen_drag",
        "Press the mouse at one point, drag, and release - use it to move windows.",
        {"x1": {"type": "integer"}, "y1": {"type": "integer"},
         "x2": {"type": "integer"}, "y2": {"type": "integer"}},
        ["x1", "y1", "x2", "y2"],
    ),
    _schema("screen_scroll", "Scroll the window under a position.", {"x": {"type": "integer"}, "y": {"type": "integer"}, "dy": {"type": "integer"}}, ["x", "y", "dy"]),
    _schema("screen_type", "Type text into the focused window, as if on the keyboard.", {"text": {"type": "string"}}, ["text"]),
    _schema(
        "screen_key",
        "Press one named key in the focused window.",
        {"key": {"type": "string", "description": "Enter, Backspace, Tab, Up, Down, Left, Right, PageUp, PageDown, Home, End, Escape"}},
        ["key"],
    ),
    _schema("window_open", "Open an app window.", {"app": {"type": "string", "enum": ["terminal", "files", "editor", "sysmon", "agent"]}, "path": {"type": "string", "description": "file to show, for editor"}}, ["app"]),
    _schema("window_list", "List open windows with their ids, apps, titles and rectangles.", {}),
    _schema("window_focus", "Focus a window by id, or the most recent window of an app. If that app has no window open, one is opened (same as clicking its taskbar button).", {"id": {"type": "integer"}, "app": {"type": "string"}}),
    _schema("window_close", "Close a window by id, or the most recent window of an app.", {"id": {"type": "integer"}, "app": {"type": "string"}}),
    _schema(
        "task_complete",
        "Finish the task and report back to the human.",
        {"summary": {"type": "string"}},
        ["summary"],
    ),
]

TERMINAL_TOOLS = {"task_complete"}


def _json_result(payload) -> str:
    text = payload if isinstance(payload, str) else json.dumps(payload, ensure_ascii=False, default=str)
    if len(text) > MAX_RESULT:
        text = text[:MAX_RESULT] + "\n... (truncated)"
    return text


class Toolbox:
    """Executes tool calls against the kernel and records them in the audit log."""

    def __init__(self, kernel):
        self.kernel = kernel
        self.shot_index = 0
        self.calls = 0

    # -- helpers -----------------------------------------------------------
    def _find_window(self, args: dict, open_if_missing: bool = False):
        wid = args.get("id")
        if wid is not None:
            win = self.kernel.screen.by_id(int(wid))
            if win is None:
                raise VfsError(f"no window with id {wid}")
            return win
        app = args.get("app")
        if not app:
            raise VfsError("provide either id or app")
        app = str(app)
        matches = [w for w in self.kernel.screen.windows if w.app == app]
        if not matches:
            if not open_if_missing:
                raise VfsError(f"no open window for app '{app}' (use window_open)")
            # same affordance as the taskbar: focusing a closed app opens it
            win = self.kernel.open_app(app)
            self.kernel.emit("window", f"window_focus opened {app} window #{win.id}")
            return win
        win = matches[-1]
        self.kernel.screen.focus_window(win.id)
        return win

    def _click(self, x: int, y: int, button: str = "left", double: bool = False) -> dict:
        scr = self.kernel.screen
        w, h = scr.width, scr.height
        if not (0 <= x < w and 0 <= y < h):
            raise VfsError(f"click ({x},{y}) is outside the screen (0,0)-({w - 1},{h - 1})")
        scr.mouse("move", x, y)
        scr.mouse("down", x, y, button)
        scr.mouse("up", x, y, button)
        if double:
            scr.mouse("double_click", x, y, button)
        return {"clicked": [x, y], "button": button, "double": bool(double)}

    # -- dispatch ----------------------------------------------------------
    def execute(self, name: str, args: dict) -> tuple[bool, str]:
        self.calls += 1
        handler = getattr(self, f"t_{name}", None)
        if handler is None:
            return False, f"unknown tool '{name}'"
        try:
            result = handler(**args)
        except TypeError as exc:
            return False, f"bad arguments for {name}: {exc}"
        except VfsError as exc:
            return False, f"{name} failed: {exc}"
        except Exception as exc:  # keep the agent alive on any tool bug
            return False, f"{name} crashed: {type(exc).__name__}: {exc}"
        ok = True
        if isinstance(result, dict) and result.get("__error__"):
            ok = False
            result = result["__error__"]
        return ok, _json_result(result)

    # -- filesystem --------------------------------------------------------
    def t_sys_info(self, **_) -> dict:
        info = self.kernel.sysinfo()
        info.pop("root", None)
        return info

    def t_fs_list(self, path: str = "/", **_) -> dict:
        entries = self.kernel.vfs.list_dir(path)
        return {"path": path, "count": len(entries), "entries": entries}

    def t_fs_read(self, path: str, **_) -> dict:
        text = self.kernel.vfs.read_text(path)
        return {"path": path, "bytes": len(text), "content": text}

    def t_fs_write(self, path: str, content: str, append: bool = False, **_) -> dict:
        res = self.kernel.vfs.write_text(path, content, append=append)
        self.kernel.emit("fs", f"wrote {res['bytes_written']} bytes to {res['path']}")
        if hasattr(self.kernel, "refresh_editors"):
            self.kernel.refresh_editors(res["path"])
        return res

    def t_fs_mkdir(self, path: str, **_) -> dict:
        res = self.kernel.vfs.mkdir(path)
        self.kernel.emit("fs", f"mkdir {path}")
        return res

    def t_fs_delete(self, path: str, recursive: bool = False, **_) -> dict:
        res = self.kernel.vfs.remove(path, recursive=recursive)
        self.kernel.emit("fs", f"deleted {path}")
        if hasattr(self.kernel, "refresh_open_windows"):
            self.kernel.refresh_open_windows()
        return res

    def t_fs_move(self, src: str, dst: str, **_) -> dict:
        res = self.kernel.vfs.move(src, dst)
        self.kernel.emit("fs", f"moved {src} -> {dst}")
        return res

    def t_fs_stat(self, path: str, **_) -> dict:
        return self.kernel.vfs.stat(path)

    def t_fs_search(self, pattern: str, path: str = "/", **_) -> dict:
        hits = self.kernel.vfs.search(pattern, path)
        return {"pattern": pattern, "hits": len(hits), "matches": hits}

    def t_shell_run(self, command: str, **_) -> dict:
        result = self.kernel.shell_exec(self.kernel.main_session, command)
        return {"command": command, "exit_code": result.code, "output": result.text}

    # -- screen ------------------------------------------------------------
    def t_screen_describe(self, **_) -> str:
        return self.kernel.screen.describe_text()

    def t_screen_screenshot(self, label: str = "", **_) -> dict:
        self.shot_index += 1
        png = self.kernel.screen.screenshot_png(force=True)
        safe = "".join(c if c.isalnum() or c in "-_" else "-" for c in label)[:40] or "screen"
        name = f"shot-{time.strftime('%H%M%S')}-{self.shot_index:03d}-{safe}.png"
        path = config.SHOTS_DIR / name
        path.write_bytes(png)
        self.kernel.emit("screenshot", f"saved {name} ({len(png)} bytes)", file=name)
        return {
            "saved": name,
            "url": f"/shots/{name}",
            "bytes": len(png),
            "note": "PNG saved for the human. Read the screen as text with screen_describe.",
            "screen_text": "\n".join(self.kernel.screen.describe_text().splitlines()[:18]),
        }

    def t_screen_move(self, x: int, y: int, **_) -> dict:
        self.kernel.screen.mouse("move", int(x), int(y))
        return {"cursor": [int(x), int(y)], "at": self.kernel.screen.top_at(int(x), int(y)).title
                if self.kernel.screen.top_at(int(x), int(y)) else None}

    def t_screen_click(self, x: int, y: int, button: str = "left", double: bool = False, **_) -> dict:
        res = self._click(int(x), int(y), button, double)
        res["screen"] = self.kernel.screen.describe_text().splitlines()[:6]
        return res

    def t_screen_drag(self, x1: int, y1: int, x2: int, y2: int, **_) -> dict:
        scr = self.kernel.screen
        for x, y in ((x1, y1), (x2, y2)):
            if not (0 <= x < scr.width and 0 <= y < scr.height):
                raise VfsError(f"({x},{y}) is outside the screen")
        scr.mouse("move", int(x1), int(y1))
        scr.mouse("down", int(x1), int(y1))
        steps = 6
        for i in range(1, steps + 1):
            scr.mouse("move", int(x1 + (x2 - x1) * i / steps), int(y1 + (y2 - y1) * i / steps))
        scr.mouse("up", int(x2), int(y2))
        return {"dragged": [x1, y1, x2, y2]}

    def t_screen_scroll(self, x: int, y: int, dy: int, **_) -> dict:
        self.kernel.screen.mouse("move", int(x), int(y))
        self.kernel.screen.mouse("wheel", int(x), int(y), "left", int(dy))
        return {"scrolled": [x, y, dy]}

    def t_screen_type(self, text: str, **_) -> dict:
        n = self.kernel.screen.type_text(text)
        focused = self.kernel.screen.focus
        return {"typed_chars": n, "into": focused.title if focused else None,
                "hint": "nothing is focused - open or focus a window first" if not focused else ""}

    _KEYS = {
        "enter": "Enter", "return": "Enter", "backspace": "Backspace", "tab": "Tab",
        "up": "Up", "down": "Down", "left": "Left", "right": "Right", "escape": "Escape",
        "esc": "Escape", "pageup": "PageUp", "pagedown": "PageDown", "home": "Home",
        "end": "End", "space": " ", "delete": "Backspace",
    }

    def t_screen_key(self, key: str, **_) -> dict:
        name = self._KEYS.get(str(key).strip().lower(), key)
        self.kernel.screen.key(name, name if len(name) == 1 else None)
        return {"pressed": name}

    # -- windows -----------------------------------------------------------
    def t_window_list(self, **_) -> dict:
        return {"windows": self.kernel.screen.describe()["windows"],
                "focused": self.kernel.screen.focus_id}

    def t_window_open(self, app: str, path: str = "", **_) -> dict:
        if app == "editor":
            win = self.kernel.open_editor(path or "/home/user/README.md")
        else:
            win = self.kernel.open_app(app)
        return {"id": win.id, "app": win.app, "title": win.title,
                "rect": [win.x, win.y, win.w, win.h]}

    def t_window_focus(self, id: int | None = None, app: str = "", **_) -> dict:
        win = self._find_window({"id": id, "app": app}, open_if_missing=True)
        return {"focused": win.id, "app": win.app, "title": win.title}

    def t_window_close(self, id: int | None = None, app: str = "", **_) -> dict:
        win = self._find_window({"id": id, "app": app})
        self.kernel.close_window(win.id)
        return {"closed": win.id, "app": win.app}

    def t_task_complete(self, summary: str, **_) -> dict:
        return {"completed": True, "summary": summary}
