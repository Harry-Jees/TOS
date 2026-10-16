"""SandboxOS desktop app - a native window, no browser required.

One window: the live virtual screen on the left (click it and your real mouse
and keyboard drive the simulated machine), and tabs on the right for the agent
feed, machine state, screenshots and the audit log. Menus control the machine:
self-test, factory reset, model key, windows and scale.

    python run.py --desktop        # or desktop.bat
    python desktop.py [--scale 0.5] [--task "..."]
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from agentd import AgentRunner, build_provider  # noqa: E402
from sandboxos import config  # noqa: E402
from sandboxos.kernel import Kernel  # noqa: E402
from sandboxos.selftest import format_report, run_self_test  # noqa: E402

APP_TITLE = "SandboxOS 1.0"
BG = "#12141b"
BG2 = "#1b1f29"
BG3 = "#232836"
FG = "#e2e6ef"
MUTED = "#8c94a4"
ACCENT = "#5ca8ff"
OK = "#70c87a"
ERR = "#f07070"

FEED_COLORS = {
    "thought": ACCENT,
    "tool": OK,
    "result": "#c8cdd8",
    "error": ERR,
    "system": MUTED,
    "window": MUTED,
    "screenshot": "#f0b652",
    "agent": "#d7b3ff",
    "fs": OK,
    "shell": "#b8e6be",
}

NAMED_KEYS = {
    "\r": "Enter",
    "\n": "Enter",
    "\x08": "Backspace",
    "\t": "Tab",
    "\x1b": "Escape",
    "Up": "Up",
    "Down": "Down",
    "Left": "Left",
    "Right": "Right",
    "Prior": "PageUp",
    "Next": "PageDown",
    "Home": "Home",
    "End": "End",
    "Delete": "Backspace",
}
TK_KEYSYM = {
    "Return": "Enter", "KP_Enter": "Enter", "BackSpace": "Backspace", "Tab": "Tab",
    "Escape": "Escape", "Up": "Up", "Down": "Down", "Left": "Left", "Right": "Right",
    "Prior": "PageUp", "Next": "PageDown", "Home": "Home", "End": "End",
    "Delete": "Backspace", "space": " ",
}


def open_path(path: pathlib.Path) -> None:
    """Open a folder/file in the host's file manager (desktop affordance only)."""
    try:
        if sys.platform.startswith("win"):
            os.startfile(str(path))  # noqa: S606
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(path)])
        else:
            subprocess.Popen(["xdg-open", str(path)])
    except Exception:
        pass


class SandboxDesktop:
    """The application window and its event pump."""

    def __init__(self, kernel: Kernel, runner: AgentRunner, scale: float | None = None):
        self.kernel = kernel
        self.runner = runner
        self.scale = scale if scale else (1.0 if self._screen_fits() else 0.5)
        self._photo = None
        self._last_seq = -1
        self._last_events = 0
        self._last_audit = 0.0
        self._shot_names: set[str] = set()
        self._autoscroll = True
        self._closed = False

        self.root = tk.Tk()
        self.root.title(f"{APP_TITLE} - sandboxed machine with an AI agent inside")
        self.root.configure(bg=BG)
        self.root.minsize(720, 520)
        self._build_style()
        self._build_menu()
        self._build_layout()
        self._bind_screen_input()
        self.root.protocol("WM_DELETE_WINDOW", self.quit)
        self._log("system", f"{APP_TITLE} booted - disk {kernel.root}")
        self._log("system", f"agent brain: {getattr(self.runner.provider, 'description', '')}")
        if not config.api_key():
            self._log("system", "no model key set yet: Machine -> Set NVIDIA API key... "
                                "(running the built-in scripted brain for now)")
        self.root.after(60, self._tick)

    # -- setup -------------------------------------------------------------
    def _screen_fits(self) -> bool:
        try:
            probe = tk.Tk()
            w, h = probe.winfo_screenwidth(), probe.winfo_screenheight()
            probe.destroy()
            return w >= self.kernel.screen.width + 420 and h >= self.kernel.screen.height + 220
        except Exception:
            return True

    def _build_style(self) -> None:
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure(".", background=BG, foreground=FG, fieldbackground=BG3, bordercolor=BG3)
        style.configure("TFrame", background=BG)
        style.configure("TLabel", background=BG, foreground=FG)
        style.configure("Muted.TLabel", foreground=MUTED)
        style.configure("Accent.TLabel", foreground=ACCENT)
        style.configure("TButton", background=BG3, foreground=FG, borderwidth=1, padding=4)
        style.map("TButton", background=[("active", "#2c3446")])
        style.configure("Run.TButton", background="#1d4f8a")
        style.map("Run.TButton", background=[("active", "#2a6bb0")])
        style.configure("TNotebook", background=BG, borderwidth=0)
        style.configure("TNotebook.Tab", background=BG2, foreground=MUTED, padding=(10, 5))
        style.map("TNotebook.Tab", background=[("selected", BG3)], foreground=[("selected", FG)])
        style.configure("TEntry", fieldbackground="#0e1015", foreground=FG, insertcolor=FG)
        style.configure("Horizontal.TProgressbar", background=OK, troughcolor=BG3)

    def _build_menu(self) -> None:
        menubar = tk.Menu(self.root)

        machine = tk.Menu(menubar, tearoff=0)
        machine.add_command(label="Set NVIDIA / OpenAI-compatible API key...", command=self.dialog_set_key)
        machine.add_command(label="Reload model settings", command=self.reload_model)
        machine.add_separator()
        machine.add_command(label="Self-test (verify the sandbox boundary)", command=self.dialog_self_test)
        machine.add_command(label="Factory reset (wipe the sandbox disk)", command=self.factory_reset)
        machine.add_separator()
        machine.add_command(label="Quit", command=self.quit)
        menubar.add_cascade(label="Machine", menu=machine)

        windows = tk.Menu(menubar, tearoff=0)
        for app, label in (("terminal", "Terminal"), ("files", "Files"), ("editor", "Editor"),
                           ("sysmon", "System Monitor"), ("agent", "Agent Console")):
            windows.add_command(label=f"Open {label}", command=lambda a=app: self.open_app(a))
        windows.add_separator()
        windows.add_command(label="Close all windows", command=self.close_all)
        menubar.add_cascade(label="Windows", menu=windows)

        view = tk.Menu(menubar, tearoff=0)
        view.add_radiobutton(label="Screen 1:1", value=1.0, variable=tk.DoubleVar(value=self.scale),
                             command=lambda: self.set_scale(1.0))
        view.add_radiobutton(label="Screen 50%", value=0.5, variable=tk.DoubleVar(value=self.scale),
                             command=lambda: self.set_scale(0.5))
        view.add_separator()
        view.add_checkbutton(label="Agent feed auto-scroll", command=self.toggle_autoscroll)
        menubar.add_cascade(label="View", menu=view)

        helpm = tk.Menu(menubar, tearoff=0)
        helpm.add_command(label="About / status", command=self.dialog_about)
        helpm.add_command(label="Open sandbox disk folder", command=lambda: open_path(self.kernel.root))
        helpm.add_command(label="Open audit log folder", command=lambda: open_path(self.kernel.audit.path.parent))
        helpm.add_command(label="Open screenshots folder", command=lambda: open_path(config.SHOTS_DIR))
        menubar.add_cascade(label="Help", menu=helpm)
        self.root.config(menu=menubar)

    def _build_layout(self) -> None:
        bar = ttk.Frame(self.root, padding=(8, 6))
        bar.pack(fill="x")
        ttk.Label(bar, text=APP_TITLE, style="Accent.TLabel").pack(side="left")
        self.task_var = tk.StringVar()
        entry = ttk.Entry(bar, textvariable=self.task_var)
        entry.pack(side="left", fill="x", expand=True, padx=8)
        entry.bind("<Return>", lambda e: self.start_task())
        entry.insert(0, "give the agent a task, e.g. tidy /home/user and prove it on screen")
        entry.bind("<FocusIn>", lambda e: entry.selection_range(0, "end") if entry.get().startswith("give the agent") else None)
        ttk.Button(bar, text="Run task", style="Run.TButton", command=self.start_task).pack(side="left")
        ttk.Button(bar, text="Abort", command=self.runner.abort).pack(side="left", padx=4)
        self.brain_label = ttk.Label(bar, style="Muted.TLabel", text="")
        self.brain_label.pack(side="left", padx=8)

        body = ttk.Frame(self.root, padding=(8, 0))
        body.pack(fill="both", expand=True)

        left = ttk.Frame(body)
        left.pack(side="left", fill="both", expand=True)
        self.canvas = tk.Canvas(left, width=int(self.kernel.screen.width * self.scale),
                                height=int(self.kernel.screen.height * self.scale),
                                highlightthickness=1, highlightbackground="#2e3444",
                                bg="#000000", bd=0)
        self.canvas.pack(anchor="nw")
        ttk.Label(left, style="Muted.TLabel",
                  text="Click the screen to focus it, then type: your mouse and keyboard drive the same "
                       "machine the agent uses. Drag title bars to move windows.").pack(anchor="w", pady=(4, 0))
        self._canvas_image = self.canvas.create_image(0, 0, anchor="nw")

        right = ttk.Frame(body, width=430)
        right.pack(side="left", fill="both", padx=(8, 0))
        right.pack_propagate(False)
        self.tabs = ttk.Notebook(right)
        self.tabs.pack(fill="both", expand=True)

        self.feed = self._make_text_tab("Agent")
        self.system = self._make_text_tab("System")
        self._build_shots_tab()
        self.audit = self._make_text_tab("Audit")

        bottom = ttk.Frame(self.root, padding=(8, 6))
        bottom.pack(fill="x")
        ttk.Label(bottom, text="shell>", style="Accent.TLabel").pack(side="left")
        self.shell_var = tk.StringVar()
        shell_entry = ttk.Entry(bottom, textvariable=self.shell_var)
        shell_entry.pack(side="left", fill="x", expand=True, padx=6)
        shell_entry.bind("<Return>", lambda e: self.run_shell())
        ttk.Button(bottom, text="Run", command=self.run_shell).pack(side="left")
        self.status = ttk.Label(self.root, style="Muted.TLabel", anchor="w", padding=(8, 2))
        self.status.pack(fill="x")
        self.progress = ttk.Progressbar(self.root, mode="determinate", maximum=100)
        self.progress.pack(fill="x", padx=8, pady=(0, 8))

    def _make_text_tab(self, name: str) -> tk.Text:
        frame = ttk.Frame(self.tabs)
        self.tabs.add(frame, text=name)
        text = tk.Text(frame, bg="#0e1015", fg=FG, insertbackground=FG, wrap="word",
                       relief="flat", font=("Consolas", 9), padx=6, pady=4)
        scroll = ttk.Scrollbar(frame, command=text.yview)
        text.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        text.pack(fill="both", expand=True)
        text.configure(state="disabled")
        return text

    def _build_shots_tab(self) -> None:
        self.shots_frame = ttk.Frame(self.tabs)
        self.tabs.add(self.shots_frame, text="Screenshots")
        self.shots_grid = ttk.Frame(self.shots_frame)
        self.shots_grid.pack(fill="both", expand=True)
        ttk.Label(self.shots_frame, style="Muted.TLabel",
                  text="What the agent captured. Click a thumbnail to open the full PNG.").pack(anchor="w")

    def _bind_screen_input(self) -> None:
        c = self.canvas
        c.bind("<Button-1>", lambda e: self._mouse("down", e))
        c.bind("<ButtonRelease-1>", lambda e: self._mouse("up", e))
        c.bind("<Button-3>", lambda e: self._mouse("down", e, button="right"))
        c.bind("<B1-Motion>", lambda e: self._mouse("move", e))
        c.bind("<Motion>", lambda e: self._mouse("move", e))
        c.bind("<Double-Button-1>", lambda e: self._mouse("double_click", e))
        c.bind("<MouseWheel>", self._wheel)
        c.bind("<Button-4>", lambda e: self._wheel(e, up=True))
        c.bind("<Button-5>", lambda e: self._wheel(e, up=False))
        c.bind("<Key>", self._key)
        c.configure(takefocus=1)

    # -- input forwarding --------------------------------------------------
    def _to_kernel(self, event) -> tuple[int, int]:
        x = int(event.x / self.scale)
        y = int(event.y / self.scale)
        return (max(0, min(self.kernel.screen.width - 1, x)),
                max(0, min(self.kernel.screen.height - 1, y)))

    def _mouse(self, action: str, event, button: str = "left") -> str:
        x, y = self._to_kernel(event)
        self.canvas.focus_set()
        result = self.kernel.human_input(
            {"type": "mouse", "action": action, "x": x, "y": y, "button": button})
        return result.get("target")

    def _wheel(self, event, up: bool | None = None) -> None:
        if up is None:
            up = getattr(event, "delta", 0) > 0
        x, y = self._to_kernel(event)
        self.kernel.human_input(
            {"type": "mouse", "action": "wheel", "x": x, "y": y, "delta": -1 if up else 1})

    def _key(self, event) -> None:
        name = TK_KEYSYM.get(event.keysym)
        if name and name != " ":
            self.kernel.human_input({"type": "key", "key": name})
            return
        if event.char and event.char.isprintable():
            self.kernel.human_input({"type": "key", "key": event.char, "text": event.char})

    # -- actions -----------------------------------------------------------
    def start_task(self) -> None:
        task = self.task_var.get().strip()
        if not task or task.startswith("give the agent a task"):
            return
        if self.runner.is_running:
            messagebox.showinfo(APP_TITLE, "The agent is still working - abort it first.")
            return
        self.kernel.notify(f"task received: {task[:80]}")
        self.runner.submit(task)
        self.task_var.set("")
        self.tabs.select(0)

    def run_shell(self) -> None:
        command = self.shell_var.get().strip()
        if not command:
            return
        result = self.kernel.shell_exec(self.kernel.main_session, command)
        self._log("shell", f"$ {command}" + (f"\n{result.text}" if result.text else ""))
        self.shell_var.set("")
        self.tabs.select(0)

    def open_app(self, app: str) -> None:
        try:
            self.kernel.open_app(app)
        except Exception as exc:
            self._log("error", f"could not open {app}: {exc}")

    def close_all(self) -> None:
        self.kernel.close_all_windows()

    def set_scale(self, scale: float) -> None:
        self.scale = scale
        self.canvas.configure(width=int(self.kernel.screen.width * scale),
                             height=int(self.kernel.screen.height * scale))
        self._last_seq = -1  # force a redraw at the new size

    def toggle_autoscroll(self) -> None:
        self._autoscroll = not self._autoscroll
        self._log("system", f"feed auto-scroll {'on' if self._autoscroll else 'off'}")

    def reload_model(self) -> None:
        self.runner.provider = build_provider(self.kernel, model=None)
        desc = getattr(self.runner.provider, "description", self.runner.provider.name)
        self._log("system", f"agent brain reloaded: {desc}")
        self._update_status()

    def factory_reset(self) -> None:
        if not messagebox.askyesno(APP_TITLE, "Wipe the sandbox disk and boot a fresh machine?\n\n"
                                             "This deletes everything inside the sandbox (not your host files)."):
            return
        self.kernel.factory_reset()
        self._last_events = 0
        self._clear(self.feed)
        self._log("system", "factory reset complete: fresh disk, fresh windows")

    def dialog_self_test(self) -> None:
        checks, failed = run_self_test(self.kernel)
        report = format_report(checks, failed)
        self._log("system", f"self-test: {len(checks) - failed}/{len(checks)} checks passed")
        win = tk.Toplevel(self.root)
        win.title("SandboxOS self-test")
        win.configure(bg=BG)
        text = tk.Text(win, bg="#0e1015", fg=FG, font=("Consolas", 9), width=86, height=min(30, len(checks) + 6))
        text.pack(fill="both", expand=True, padx=8, pady=8)
        text.insert("1.0", report)
        text.configure(state="disabled")
        ttk.Button(win, text="Close", command=win.destroy).pack(pady=(0, 8))

    def dialog_about(self) -> None:
        key = config.api_key()
        source = next((n for n in config.ENV_KEY_NAMES if config.get(n)), None)
        model = config.get("SANDBOX_LLM_MODEL", config.DEFAULT_MODEL)
        lines = [
            f"{APP_TITLE} (Python {sys.version.split()[0]}, Tk {tk.TkVersion})",
            "",
            f"agent brain : {getattr(self.runner.provider, 'description', self.runner.provider.name)}",
            f"model key   : {'set from ' + str(source) if key else 'not set (scripted brain)'}",
            f"base url    : {config.base_url()}",
            f"model       : {model}",
            "",
            f"sandbox disk: {self.kernel.root}",
            f"audit log   : {self.kernel.audit.path}",
            f"screenshots : {config.SHOTS_DIR}",
            f"virtual scr : {self.kernel.screen.width}x{self.kernel.screen.height}",
            f"quota       : {self.kernel.vfs.quota_bytes} bytes",
            "",
            "The agent's tools can only reach inside the sandbox: no host binaries,",
            "no host filesystem, no network. Your key is read from .env and never",
            "printed or logged.",
        ]
        messagebox.showinfo(APP_TITLE, "\n".join(lines))

    def dialog_set_key(self) -> None:
        win = tk.Toplevel(self.root)
        win.title("Set model API key")
        win.configure(bg=BG)
        win.transient(self.root)
        ttk.Label(win, style="Muted.TLabel", wraplength=520, justify="left",
                  text=("Paste your NVIDIA NIM key (nvapi-...) or any OpenAI-compatible key.\n"
                        "It is written to the project's .env file and never shown in logs, the\n"
                        "agent feed or the audit log.")).pack(anchor="w", padx=12, pady=(12, 4))
        var = tk.StringVar()
        entry = ttk.Entry(win, textvariable=var, show="*", width=64)
        entry.pack(padx=12, pady=4)
        entry.focus_set()
        result = ttk.Label(win, style="Muted.TLabel", text="", wraplength=520, justify="left")
        result.pack(anchor="w", padx=12, pady=4)
        row = ttk.Frame(win)
        row.pack(pady=(4, 12))

        def save() -> None:
            value = var.get().strip()
            if not value:
                result.configure(text="Nothing to save.", foreground=ERR)
                return
            try:
                write_env_key(value)
            except Exception as exc:
                result.configure(text=f"Could not write .env: {exc}", foreground=ERR)
                return
            self.kernel.audit.record("api_key_set", actor="desktop", target=".env")
            self.reload_model()
            var.set("")
            result.configure(text="Saved to .env. Brain reloaded.", foreground=OK)
            self._log("system", "API key saved to .env (value never logged); brain reloaded")

        def test() -> None:
            value = var.get().strip()
            result.configure(text="Testing the endpoint...", foreground=MUTED)

            def work() -> None:
                try:
                    if value:
                        from agentd.providers import OpenAICompatProvider

                        provider = OpenAICompatProvider(value)
                    else:
                        provider = self.runner.provider
                    reply = provider.complete_text(
                        [{"role": "user", "content": "Reply with the single word: ready"}])
                    message = f"OK - {provider.description} replied: {reply.strip()[:60]}"
                    colour = OK
                except Exception as exc:
                    message = f"Failed: {exc}"
                    colour = ERR
                self.root.after(0, lambda: result.configure(text=message, foreground=colour))

            threading.Thread(target=work, daemon=True).start()

        ttk.Button(row, text="Save to .env and reload", style="Run.TButton", command=save).pack(side="left")
        ttk.Button(row, text="Test connection", command=test).pack(side="left", padx=6)
        ttk.Button(row, text="Cancel", command=win.destroy).pack(side="left")
        entry.bind("<Return>", lambda e: save())

    def quit(self) -> None:
        self._closed = True
        try:
            self.root.destroy()
        except tk.TclError:
            pass

    # -- rendering / event pump -------------------------------------------
    def _tick(self) -> None:
        if self._closed:
            return
        try:
            self._render_screen()
            self._pump_events()
            self._refresh_shots()
            now = time.time()
            if now - self._last_audit > 2.0:
                self._last_audit = now
                self._refresh_audit()
            self._update_status()
        except tk.TclError:
            return
        self.root.after(70, self._tick)

    def _render_screen(self) -> None:
        seq = self.kernel.screen.frame_seq
        if seq == self._last_seq:
            return
        self._last_seq = seq
        png = self.kernel.screen.screenshot_png()  # cached by frame_seq inside the kernel
        image = tk.PhotoImage(data=png)
        if self.scale < 1.0:
            image = image.subsample(int(round(1 / self.scale)))
        elif self.scale > 1.0:
            image = image.zoom(int(self.scale))
        self._photo = image  # keep a reference or Tk garbage-collects the image
        self.canvas.itemconfigure(self._canvas_image, image=self._photo)

    def _pump_events(self) -> None:
        events = self.kernel.events_since(self._last_events)
        if not events:
            return
        self._last_events = events[-1]["seq"]
        for ev in events:
            self._log(ev["kind"], ev["text"], ts=ev.get("ts"))
        if self._autoscroll:
            self.feed.see("end")

    def _refresh_shots(self) -> None:
        shots = sorted(config.SHOTS_DIR.glob("*.png"))
        new = [p for p in shots if p.name not in self._shot_names]
        for path in new[-6:]:
            self._shot_names.add(path.name)
            try:
                data = path.read_bytes()
                image = tk.PhotoImage(data=data).subsample(4)
            except Exception:
                continue
            holder = ttk.Frame(self.shots_grid)
            holder.pack(side="top", anchor="w", pady=2)
            label = tk.Label(holder, image=image, bg="#000000", cursor="hand2")
            label.image = image  # keep reference
            label.pack(side="left")
            ttk.Label(holder, style="Muted.TLabel", text=path.name,
                      wraplength=180).pack(side="left", padx=6)
            label.bind("<Button-1>", lambda e, p=path: open_path(p))

    def _refresh_audit(self) -> None:
        entries = self.kernel.audit.tail(40)
        lines = []
        for e in entries:
            when = time.strftime("%H:%M:%S", time.localtime(e.get("ts", 0)))
            detail = e.get("command") or e.get("tool") or e.get("task") or e.get("app") or ""
            lines.append(f"{when}  {e.get('actor','?'):<7} {e.get('action',''):<16} {str(detail)[:60]}")
        self._set_text(self.audit, "\n".join(lines))

    def _update_status(self) -> None:
        info = self.kernel.sysinfo()
        d, a = info["disk"], info["agent"]
        self.brain_label.configure(text=f"brain: {getattr(self.runner.provider, 'name', '?')}")
        self.status.configure(
            text=(f"agent: {a['state']} ({a['steps']} steps)   windows: {info['windows']}   "
                  f"files: {d['files']}   disk: {d['used_bytes']}/{d['quota_bytes']} B   "
                  f"uptime: {info['uptime_human']}" + (f"   task: {a['task'][:48]}" if a["task"] else "")))
        pct = min(100, int(100 * d["used_bytes"] / max(1, d["quota_bytes"])))
        self.progress.configure(value=pct)

    # -- text helpers ------------------------------------------------------
    def _clear(self, widget: tk.Text) -> None:
        widget.configure(state="normal")
        widget.delete("1.0", "end")
        widget.configure(state="disabled")

    def _set_text(self, widget: tk.Text, content: str) -> None:
        widget.configure(state="normal")
        widget.delete("1.0", "end")
        widget.insert("1.0", content)
        widget.configure(state="disabled")

    def _log(self, kind: str, text: str, ts: float | None = None) -> None:
        when = time.strftime("%H:%M:%S", time.localtime(ts or time.time()))
        self.feed.configure(state="normal")
        self.feed.insert("end", f"{when} {kind}\n", kind)
        self.feed.insert("end", f"{text}\n", kind)
        self.feed.insert("end", "\n")
        self.feed.tag_configure(kind, foreground=FEED_COLORS.get(kind, FG))
        if int(self.feed.index("end-1c").split(".")[0]) > 1200:
            self.feed.delete("1.0", "400.0")
        self.feed.configure(state="disabled")
        if self._autoscroll:
            self.feed.see("end")

    # -- run ---------------------------------------------------------------
    def run(self) -> int:
        self.root.mainloop()
        return 0


def write_env_key(value: str, env_path: pathlib.Path | None = None) -> pathlib.Path:
    """Store the key in .env without disturbing other settings."""
    env_path = env_path or config.ENV_FILE
    line = f"SANDBOX_LLM_API_KEY={value.strip()}"
    existing: list[str] = []
    if env_path.exists():
        for old in env_path.read_text(encoding="utf-8").splitlines():
            if not old.strip().startswith("SANDBOX_LLM_API_KEY="):
                existing.append(old)
    existing.append(line)
    text = "\n".join(existing).lstrip("\n")
    if not text.endswith("\n"):
        text += "\n"
    env_path.write_text(text, encoding="utf-8")
    os.environ["SANDBOX_LLM_API_KEY"] = value.strip()  # pick it up without a restart
    return env_path


def build_app(scale: float | None = None, boot_windows: bool = True) -> SandboxDesktop:
    config.ensure_dirs()
    kernel = Kernel(boot_windows=boot_windows)
    provider = build_provider(kernel)
    runner = AgentRunner(kernel, provider=provider)
    return SandboxDesktop(kernel, runner, scale=scale)


def run(kernel: Kernel, runner: AgentRunner, scale: float | None = None) -> int:
    """Entry point used by run.py --desktop."""
    return SandboxDesktop(kernel, runner, scale=scale).run()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="SandboxOS desktop app")
    parser.add_argument("--scale", type=float, default=None, help="1.0 or 0.5 screen scale")
    parser.add_argument("--task", help="hand the agent a task on startup")
    parser.add_argument("--scripted", action="store_true", help="force the offline brain")
    args = parser.parse_args(argv)

    config.ensure_dirs()
    kernel = Kernel(boot_windows=True)
    provider = build_provider(kernel, force="scripted" if args.scripted else None)
    runner = AgentRunner(kernel, provider=provider)
    if args.task:
        runner.submit(args.task)
    return SandboxDesktop(kernel, runner, scale=args.scale).run()


if __name__ == "__main__":
    raise SystemExit(main())
