"""Local control server for SandboxOS.

Serves the browser console: a live view of the virtual screen, the agent chat,
the event feed and the screenshot gallery. Binds to loopback only.

Endpoints
  GET  /                 console UI
  GET  /style.css, /app.js
  GET  /screen.png       current frame (PNG)
  GET  /state?since=N    machine state + new events
  GET  /audit?n=50       host-side audit log
  GET  /shots/<name>     saved screenshots
  POST /prompt           {"task": "..."}  -> hand the agent a task
  POST /abort            stop the agent
  POST /input            {"event": {...}} human mouse/keyboard into the sandbox
  POST /open             {"app": "terminal"} open an app window
  POST /shell            {"command": "ls"} run a command in the main session
"""

from __future__ import annotations

import json
import pathlib
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from sandboxos import config


class ControlServer:
    def __init__(self, kernel, runner, host: str = "127.0.0.1", port: int = 8787):
        self.kernel = kernel
        self.runner = runner
        self.host = host
        self.ports_to_try = list(range(port, port + 20))
        self.httpd: ThreadingHTTPServer | None = None
        self.port: int | None = None
        self._thread: threading.Thread | None = None
        self.web_dir = config.WEB_DIR

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> int:
        last_error: Exception | None = None
        for port in self.ports_to_try:
            try:
                handler = self._make_handler()
                self.httpd = ThreadingHTTPServer((self.host, port), handler)
                self.httpd.daemon_threads = True
                self.port = port
                break
            except OSError as exc:
                last_error = exc
                continue
        if self.httpd is None or self.port is None:
            raise RuntimeError(f"could not bind a local port: {last_error}")
        self.kernel.http_port = self.port
        self._thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self._thread.start()
        self.kernel.emit("system", f"control server listening on http://{self.host}:{self.port}")
        return self.port

    def stop(self) -> None:
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
            self.httpd = None

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}"

    # -- handler -----------------------------------------------------------
    def _make_handler(self):
        outer = self

        class Handler(BaseHTTPRequestHandler):
            server_version = "SandboxOS/1.0"
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *args):  # keep the console quiet
                if outer.kernel.sysinfo()["agent"]["state"] != "running":
                    return
                return

            # -- helpers ---------------------------------------------------
            def _send(self, code: int, body: bytes, ctype: str, cache: str = "no-store"):
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", cache)
                self.send_header("X-Content-Type-Options", "nosniff")
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def _json(self, payload, code: int = 200):
                self._send(code, json.dumps(payload, default=str).encode("utf-8"),
                           "application/json; charset=utf-8")

            def _read_json(self) -> dict:
                length = int(self.headers.get("Content-Length") or 0)
                if not length:
                    return {}
                raw = self.rfile.read(length)
                try:
                    data = json.loads(raw.decode("utf-8"))
                    return data if isinstance(data, dict) else {}
                except (ValueError, UnicodeDecodeError):
                    return {}

            # -- GET -------------------------------------------------------
            def do_GET(self):
                parsed = urlparse(self.path)
                path = parsed.path
                query = parse_qs(parsed.query)
                if path in ("/", "/index.html"):
                    return self._serve_file(outer.web_dir / "index.html", "text/html; charset=utf-8")
                if path in ("/app.js", "/style.css"):
                    ctype = "application/javascript; charset=utf-8" if path.endswith(".js") else "text/css; charset=utf-8"
                    return self._serve_file(outer.web_dir / path.lstrip("/"), ctype)
                if path == "/screen.png":
                    png = outer.kernel.screen.screenshot_png()
                    return self._send(200, png, "image/png")
                if path == "/state":
                    since = int((query.get("since") or ["0"])[0] or 0)
                    return self._json({
                        "ok": True,
                        "time": time.time(),
                        "sysinfo": outer.kernel.sysinfo(),
                        "screen": {"w": outer.kernel.screen.width, "h": outer.kernel.screen.height,
                                   "seq": outer.kernel.screen.frame_seq},
                        "windows": outer.kernel.window_list(),
                        "events": outer.kernel.events_since(since),
                        "notifications": outer.kernel.notifications(),
                        "provider": getattr(outer.runner.provider, "description", outer.runner.provider.name),
                        "provider_name": outer.runner.provider.name,
                        "running": outer.runner.is_running,
                        "shots": outer.list_shots()[-12:],
                    })
                if path == "/audit":
                    n = int((query.get("n") or ["50"])[0] or 50)
                    return self._json({"ok": True, "entries": outer.kernel.audit.tail(n)})
                if path.startswith("/shots/"):
                    return self._serve_shot(path.split("/", 2)[-1])
                return self._json({"ok": False, "error": "not found"}, 404)

            # -- POST ------------------------------------------------------
            def do_POST(self):
                path = urlparse(self.path).path
                data = self._read_json()
                if path == "/prompt":
                    task = str(data.get("task") or "").strip()
                    if not task:
                        return self._json({"ok": False, "error": "task is empty"}, 400)
                    if outer.runner.is_running:
                        return self._json({"ok": False, "error": "agent is already working"}, 409)
                    outer.kernel.notify(f"task received: {task[:80]}")
                    outer.runner.submit(task)
                    return self._json({"ok": True, "task": task})
                if path == "/abort":
                    outer.runner.abort()
                    return self._json({"ok": True})
                if path == "/input":
                    event = data.get("event") if isinstance(data.get("event"), dict) else data
                    return self._json({"ok": True, "result": outer.kernel.human_input(event)})
                if path == "/open":
                    app = str(data.get("app") or "")
                    try:
                        win = outer.kernel.open_app(app, **({"path": data["path"]} if data.get("path") else {}))
                    except KeyError:
                        return self._json({"ok": False, "error": f"unknown app '{app}'"}, 400)
                    return self._json({"ok": True, "window": win.id})
                if path == "/shell":
                    command = str(data.get("command") or "")
                    result = outer.kernel.shell_exec(outer.kernel.main_session, command)
                    outer.kernel.screen.touch()
                    return self._json({"ok": True, "output": result.text, "code": result.code})
                return self._json({"ok": False, "error": "not found"}, 404)

            # -- file helpers ----------------------------------------------
            def _serve_file(self, path: pathlib.Path, ctype: str):
                try:
                    body = path.read_bytes()
                except OSError:
                    return self._send(404, b"missing static file", "text/plain")
                self._send(200, body, ctype, cache="no-store")

            def _serve_shot(self, name: str):
                safe = pathlib.Path(name).name
                if not safe.endswith(".png"):
                    return self._json({"ok": False, "error": "not a png"}, 400)
                target = (config.SHOTS_DIR / safe).resolve()
                if config.SHOTS_DIR.resolve() not in target.parents:
                    return self._json({"ok": False, "error": "bad path"}, 400)
                try:
                    body = target.read_bytes()
                except OSError:
                    return self._send(404, b"no such screenshot", "text/plain")
                self._send(200, body, "image/png")

        return Handler

    def list_shots(self) -> list[dict]:
        out = []
        for p in sorted(config.SHOTS_DIR.glob("*.png")):
            out.append({"name": p.name, "url": f"/shots/{p.name}", "bytes": p.stat().st_size,
                        "mtime": int(p.stat().st_mtime)})
        return out


def serve(kernel, runner, host: str = "127.0.0.1", port: int = 8787) -> ControlServer:
    server = ControlServer(kernel, runner, host=host, port=port)
    server.start()
    return server
