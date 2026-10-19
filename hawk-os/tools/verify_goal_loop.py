"""Verify the goal loop against a local mock model, offline.

Runs three scenarios against a local OpenAI-compatible mock:

  A. plan -> execute -> verify says done            (exit 0, plan executed)
  B. plan -> verify sends a correction -> done      (both executed)
  C. plan ends in `done`                            (verified before believing)

Every scenario also asserts the vision request really carried a screenshot.

    python tools/verify_goal_loop.py
"""
import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "hawk.exe")
PORT = 0

replies: list[str] = []
state = {"calls": 0, "with_image": 0, "lock": threading.Lock()}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", "replace")
        with state["lock"]:
            i = state["calls"]
            state["calls"] += 1
            if "data:image/png;base64," in body:
                state["with_image"] += 1
        reply = replies[i] if i < len(replies) else "done"
        payload = json.dumps({"choices": [{"message": {"content": reply}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def scenario(name, goal, script_replies, want_files):
    global replies
    replies = script_replies
    state["calls"] = state["with_image"] = 0
    for f in want_files:
        p = os.path.join(ROOT, "workspace", f)
        if os.path.exists(p):
            os.remove(p)
    r = subprocess.run(
        [EXE, "--base-url", f"http://127.0.0.1:{PORT}/v1", "--allow-screen-upload", "--auto-run",
         "--delay", "100", "--steps", "20", "--goal", goal],
        capture_output=True, text=True, timeout=180, errors="replace")
    out = r.stdout + r.stderr
    ok_files = all(os.path.exists(os.path.join(ROOT, "workspace", f)) for f in want_files)
    ok_calls = state["calls"] >= 2 and state["with_image"] == state["calls"]
    ok_done = "goal finished" in out
    ok = r.returncode == 0 and ok_files and ok_calls and ok_done
    detail = (f"exit={r.returncode}, vision calls={state['calls']} "
              f"all with screenshots={state['with_image'] == state['calls']}, "
              f"files={ {f: os.path.exists(os.path.join(ROOT, 'workspace', f)) for f in want_files} }")
    print(f"{name:<58} {'PASS' if ok else 'FAIL'}   {detail}")
    if not ok:
        print("  --- output tail ---")
        for line in out.splitlines()[-12:]:
            print("   ", line)
    return ok


def main():
    # Bind an ephemeral loopback port so parallel test runs cannot collide with
    # another process or accidentally send mock requests to a stale listener.
    global PORT
    server = HTTPServer(("127.0.0.1", 0), Handler)
    PORT = server.server_address[1]
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    results = []
    # The app deliberately accepts credentials only from .env. Use a temporary
    # mock key for this offline test and restore any user file byte-for-byte.
    env_path = os.path.join(ROOT, ".env")
    had_env = os.path.isfile(env_path)
    saved_env = None
    if had_env:
        with open(env_path, "rb") as f:
            saved_env = f.read()
    with open(env_path, "w", encoding="utf-8") as f:
        f.write("NVIDIA_API_KEY=mock\n")
    try:
        results.append(scenario(
            "plan -> execute -> verify 'done'",
            "write a file called mock-a.txt",
            ["write mock-a.txt planned by the model\nlog the plan is running\nwait 100", "done"],
            ["mock-a.txt"]))
        results.append(scenario(
            "verify round inserts a correction that really runs",
            "write files a and b",
            ["write mock-b.txt planned", "write mock-c.txt corrected by the model", "done"],
            ["mock-b.txt", "mock-c.txt"]))
        results.append(scenario(
            "a plan ending in 'done' is verified, not believed",
            "write a file called mock-d.txt",
            ["write mock-d.txt from a plan that ends in done\ndone"],
            ["mock-d.txt"]))
        results.append(scenario(
            "Unicode escapes are decoded from model JSON",
            "create a unicode test file",
            ["write mock-unicode.txt café 🦅", "done"],
            ["mock-unicode.txt"]))
        unicode_path = os.path.join(ROOT, "workspace", "mock-unicode.txt")
        try:
            with open(unicode_path, "r", encoding="utf-8") as f:
                unicode_ok = f.read() == "café 🦅"
        except (OSError, UnicodeError):
            unicode_ok = False
        print(f"Unicode file contents preserved: {'PASS' if unicode_ok else 'FAIL'}")
        results.append(unicode_ok)
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)
        # Restore the user's .env exactly; never leave the mock credential behind.
        if had_env and saved_env is not None:
            with open(env_path, "wb") as f:
                f.write(saved_env)
        else:
            try:
                os.remove(env_path)
            except OSError:
                pass
        # Leave the repository workspace clean even if a scenario fails.
        for name in ("mock-a.txt", "mock-b.txt", "mock-c.txt", "mock-d.txt", "mock-unicode.txt"):
            path = os.path.join(ROOT, "workspace", name)
            try:
                if os.path.isfile(path):
                    os.remove(path)
            except OSError:
                pass
    passed = sum(results)
    print(f"\n{passed}/{len(results)} checks passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
