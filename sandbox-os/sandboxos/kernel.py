"""The SandboxOS kernel.

Boots the machine, owns the parts, and provides the single trusted surface
through which anything (human or AI agent) can act: the VFS, the shell, the
window manager and the audit log. Every mutation is logged.
"""

from __future__ import annotations

import pathlib
import threading
import time

from . import config
from .audit import AuditLog
from .screen import APP_CLASSES, WindowManager
from .shell import CommandResult, ShellSession, run_line
from .vfs import Vfs

MOTD = """Welcome to SandboxOS 1.0
-----------------------
This machine is a simulation. It has its own filesystem, its own screen and
its own shell. Nothing here can reach the host computer: no host binaries, no
network, no escape. Explore freely - break whatever you like.
"""

README = """# SandboxOS

A simulated operating system with an AI agent inside it.

* The filesystem lives at / (quota-limited, jailed on the host).
* The shell is an interpreter: try `help`, `ls -l /`, `tree -L 2 /home`.
* The screen has a window manager: open windows, click buttons, type text.
* The agent console shows what the AI is thinking and doing, right now.

Files worth a look:
  /etc/motd                        this machine's welcome message
  /home/user/notes/todo.md         a to-do list
  /home/user/projects/fizzbuzz.py  a small program
  /var/log/boot.log                boot log

Anything in here is safe to delete, rewrite or destroy - it is a sandbox.
"""

TODO = """# SandboxOS demo to-do

- [x] boot the kernel
- [x] mount the jailed filesystem
- [ ] write a file from the shell
- [ ] open a window and type into it
- [ ] take a screenshot and look at it
- [ ] break something on purpose, then repair it
"""

FIZZBUZZ = '''"""Classic fizzbuzz - the agent can run this with the sandbox shell."""


def fizzbuzz(n: int) -> str:
    if n % 15 == 0:
        return "FizzBuzz"
    if n % 3 == 0:
        return "Fizz"
    if n % 5 == 0:
        return "Buzz"
    return str(n)


if __name__ == "__main__":
    print(" ".join(fizzbuzz(i) for i in range(1, 31)))
'''

BOOT_LOG = """[    0.000000] SandboxOS 1.0 booting (simulated kernel 6.0-sandbox)
[    0.000412] /dev/vfs0 mounted at /  (quota 8 MiB)
[    0.001003] virtual screen 960x600 attached
[    0.001220] window manager started: terminal, files, editor, sysmon, agent
[    0.001455] shell /bin/sssh ready
[    0.001877] audit log opened on the host side (read-only from inside)
[    0.002001] network: none by design
[    0.002144] agent bridge armed - awaiting a task
[    0.002900] boot complete
"""


class Kernel:
    def __init__(
        self,
        root: pathlib.Path | None = None,
        var_dir: pathlib.Path | None = None,
        quota_bytes: int | None = None,
        boot_windows: bool = True,
    ):
        config.ensure_dirs()
        self.root = pathlib.Path(root) if root else config.ROOT_DIR
        self.var_dir = pathlib.Path(var_dir) if var_dir else config.VAR_DIR
        self.var_dir.mkdir(parents=True, exist_ok=True)
        self.audit = AuditLog(self.var_dir / "audit.log")
        self.vfs = Vfs(
            self.root,
            quota_bytes=quota_bytes or config.FS_QUOTA_BYTES,
            max_file_bytes=config.FS_MAX_FILE_BYTES,
            max_entries=config.FS_MAX_ENTRIES,
        )
        self.screen = WindowManager(
            self, config.SCREEN_W, config.SCREEN_H, config.PANEL_H, config.TASKBAR_H
        )
        self.started_at = time.time()
        self.events: list[dict] = []
        self._ev_seq = 0
        self._lock = threading.RLock()
        self.agent_log: list[dict] = []
        self.agent_state = {"state": "idle", "steps": 0, "task": None, "started": None}
        self.agent = None  # set by the run layer (agent.runner.AgentRunner)
        self.http_port: int | None = None
        self._notifications: list[dict] = []
        self.main_session = ShellSession(self)
        self._booted = False
        self.seed_filesystem()  # the disk exists whether or not windows do
        if boot_windows:
            self.boot()

    # -- boot --------------------------------------------------------------
    def boot(self) -> None:
        if self._booted:
            return
        self._booted = True
        self.seed_filesystem()
        self.boot_windows()
        self.emit("system", "kernel boot complete: vfs mounted, screen 960x600, shell ready")
        self.audit.record("boot", actor="kernel", root=str(self.root))

    def seed_filesystem(self) -> None:
        self.vfs.mkdir("/home/user/notes")
        self.vfs.mkdir("/home/user/projects")
        self.vfs.mkdir("/tmp")
        self.vfs.mkdir("/etc")
        self.vfs.mkdir("/var/log")
        self.vfs.seed(
            {
                "/etc/motd": MOTD,
                "/etc/hostname": "sandbox\n",
                "/home/user/README.md": README,
                "/home/user/notes/todo.md": TODO,
                "/home/user/projects/fizzbuzz.py": FIZZBUZZ,
                "/var/log/boot.log": BOOT_LOG,
            }
        )

    def banner(self) -> str:
        return (
            "SandboxOS 1.0  (simulated kernel 6.0-sandbox)\n"
            f"screen {self.screen.width}x{self.screen.height}, vfs at /, no host access"
        )

    # -- events ------------------------------------------------------------
    def emit(self, kind: str, text: str, **data) -> dict:
        with self._lock:
            self._ev_seq += 1
            ev = {"seq": self._ev_seq, "ts": time.time(), "kind": kind, "text": str(text)[:800]}
            if data:
                ev["data"] = data
            self.events.append(ev)
            if len(self.events) > 5000:
                del self.events[:2000]
            if kind in ("thought", "tool", "result", "error", "agent", "screenshot", "fs"):
                self.agent_log.append({"seq": ev["seq"], "kind": kind, "text": ev["text"]})
                if len(self.agent_log) > 2000:
                    del self.agent_log[:1000]
            return ev

    def events_since(self, seq: int = 0, limit: int = 300) -> list[dict]:
        with self._lock:
            out = [e for e in self.events if e["seq"] > seq]
            return out[-limit:]

    def agent_feed(self, limit: int = 40) -> list[dict]:
        with self._lock:
            return self.agent_log[-limit:]

    def notify(self, text: str, level: str = "info") -> None:
        self._notifications.append({"text": text, "level": level, "ts": time.time()})
        del self._notifications[:-20]
        self.emit("system" if level == "info" else "error", text)

    def notifications(self) -> list[dict]:
        return list(self._notifications[-8:])

    # -- shell -------------------------------------------------------------
    def new_session(self, user: str = "user") -> ShellSession:
        """A fresh shell session - each terminal window gets its own cwd/history."""
        return ShellSession(self, user=user)

    def shell_exec(self, session: ShellSession, line: str) -> CommandResult:
        result = run_line(session, line)
        self.emit("shell", f"$ {line}" + (f" -> {result.text[:120]}" if result.text else ""),
                  code=result.code, cwd=session.cwd)
        return result

    # -- apps / windows ----------------------------------------------------
    def open_app(self, app: str, **kwargs):
        if app == "editor" and "path" not in kwargs:
            kwargs["path"] = "/home/user/README.md"
        if app not in APP_CLASSES:
            raise KeyError(app)
        win = self.screen.open(app, **kwargs)
        self.audit.record("window_open", actor="kernel", app=app, window=win.id)
        return win

    def open_editor(self, path: str):
        for win in self.screen.windows:
            if win.app == "editor" and getattr(win, "path", None) == path:
                win.load()
                self.screen.focus_window(win.id)
                return win
        return self.screen.open("editor", path=path)

    def refresh_editors(self, path: str | None = None) -> None:
        """Reload editor windows after a write (so the screen shows new content)."""
        for win in self.screen.windows:
            if win.app != "editor":
                continue
            if path is None or getattr(win, "path", None) == path:
                win.load()
        self.screen.touch()

    def refresh_open_windows(self) -> None:
        """Re-scan files windows after filesystem changes."""
        for win in self.screen.windows:
            if win.app == "files" and hasattr(win, "refresh"):
                win.refresh()
        self.refresh_editors()

    def close_window(self, wid: int) -> bool:
        ok = self.screen.close(wid)
        if ok:
            self.audit.record("window_close", actor="kernel", window=wid)
        return ok

    def focus_window(self, wid: int) -> bool:
        return self.screen.focus_window(wid)

    # -- input -------------------------------------------------------------
    def human_input(self, event: dict) -> dict:
        kind = str(event.get("type", ""))
        if kind == "mouse":
            return self.screen.mouse(
                str(event.get("action", "move")),
                int(event.get("x", 0)),
                int(event.get("y", 0)),
                str(event.get("button", "left")),
                int(event.get("delta", 0)),
            )
        if kind == "key":
            return self.screen.key(str(event.get("key", "")), event.get("text"))
        if kind == "text":
            return {"handled": True, "target": f"typed {self.screen.type_text(str(event.get('text','')))} chars"}
        return {"handled": False, "target": None}

    # -- introspection -----------------------------------------------------
    def process_table(self) -> list[dict]:
        rows = [
            {"pid": 1, "user": "root", "state": "S", "cmd": "/sbin/init"},
            {"pid": 2, "user": "root", "state": "S", "cmd": "[kthreadd]"},
            {"pid": 3, "user": "root", "state": "S", "cmd": "[vfsd]"},
            {"pid": 4, "user": "root", "state": "S", "cmd": "[wm]"},
            {"pid": 5, "user": "user", "state": "R", "cmd": "/bin/sssh (terminal)"},
            {"pid": 6, "user": "user", "state": "S", "cmd": "/usr/bin/agentd --bridge"},
        ]
        if self.agent is not None and self.agent_state["state"] == "running":
            rows.append(
                {
                    "pid": 101,
                    "user": "agent",
                    "state": "R",
                    "cmd": f"/usr/bin/agentd run --task {str(self.agent_state['task'])[:40]}",
                }
            )
        return rows

    def uptime_seconds(self) -> float:
        return time.time() - self.started_at

    def sysinfo(self) -> dict:
        secs = int(self.uptime_seconds())
        m, s = divmod(secs, 60)
        h, m = divmod(m, 60)
        u = self.vfs.usage()
        return {
            "name": "SandboxOS",
            "version": "1.0",
            "kernel": "6.0-sandbox",
            "uptime_seconds": secs,
            "uptime_human": f"{h}:{m:02d}:{s:02d}",
            "windows": len(self.screen.windows),
            "processes": len(self.process_table()),
            "screen": {"w": self.screen.width, "h": self.screen.height},
            "disk": u,
            "focused_window": self.screen.focus_id,
            "agent": {
                "state": self.agent_state["state"],
                "steps": self.agent_state["steps"],
                "task": self.agent_state["task"],
                "recent": [f"{i['kind']}: {i['text']}" for i in self.agent_feed(limit=8)],
            },
            "root": str(self.root),
            "port": self.http_port,
        }

    def window_list(self, content_lines: int = 20) -> list[dict]:
        """Structure + a compact text dump of each window (what the UI shows)."""
        out = []
        for w in self.screen.windows:
            out.append(
                {
                    "id": w.id,
                    "app": w.app,
                    "title": w.title,
                    "rect": [w.x, w.y, w.w, w.h],
                    "focused": w.id == self.screen.focus_id,
                    "content": w.describe()[:content_lines] if content_lines else [],
                }
            )
        return out

    def factory_reset(self) -> dict:
        """Wipe the sandbox disk and boot a fresh machine (host-side state stays)."""
        self.close_all_windows()
        for entry in self.vfs.list_dir("/"):
            try:
                self.vfs.remove(entry["path"], recursive=True)
            except Exception:
                pass
        self.seed_filesystem()
        self.boot_windows()
        self.emit("system", "factory reset: the sandbox disk was wiped and re-seeded")
        self.audit.record("factory_reset", actor="kernel")
        return self.vfs.usage()

    def close_all_windows(self) -> int:
        n = 0
        for win in list(self.screen.windows):
            if self.screen.close(win.id):
                n += 1
        return n

    def boot_windows(self) -> None:
        """Open the standard desktop: terminal, files, agent console, system monitor."""
        term = self.open_app("terminal")
        self.open_app("files", path="/home/user")
        self.open_app("agent")
        self.open_app("sysmon")
        self.focus_window(term.id)

    def reset_agent_state(self) -> None:
        self.agent_state.update({"state": "idle", "steps": 0, "task": None, "started": None})
