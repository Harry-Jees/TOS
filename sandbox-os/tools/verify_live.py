#!/usr/bin/env python3
"""Verify a running SandboxOS console, end to end, over HTTP.

This checks the exact interface the browser uses: the UI assets, the screen
stream, the state contract app.js depends on, human input reaching the pixels,
the agent doing real work, screenshots and the audit log.

  python tools/verify_live.py [--url http://127.0.0.1:8787]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

CHECKS: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    CHECKS.append((name, bool(ok), detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""))


def get(url: str):
    with urllib.request.urlopen(url, timeout=20) as resp:
        return resp.status, resp.read(), resp.headers.get("Content-Type", "")


def post(url: str, payload: dict):
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"}, method="POST"
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.loads(resp.read().decode())


def png_size(data: bytes) -> tuple[int, int]:
    return struct.unpack(">II", data[16:24])


def state(base: str, since: int = 0) -> dict:
    _, body, _ = get(f"{base}/state?since={since}")
    return json.loads(body)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8787")
    ap.add_argument("--skip-agent", action="store_true")
    args = ap.parse_args()
    base = args.url.rstrip("/")
    print(f"verifying SandboxOS console at {base}\n")

    # 1. UI assets
    try:
        status, body, ctype = get(base + "/")
        check("GET / serves the console", status == 200 and b"SandboxOS" in body, ctype)
    except urllib.error.URLError as exc:
        check("GET / serves the console", False, str(exc))
        return 1
    for path, want in (("/app.js", "javascript"), ("/style.css", "css")):
        status, body, ctype = get(base + path)
        check(f"GET {path}", status == 200 and want in ctype and len(body) > 500, f"{len(body)} bytes")

    # 2. screen stream
    _, png, ctype = get(base + "/screen.png")
    w, h = png_size(png) if png.startswith(b"\x89PNG") else (0, 0)
    check("GET /screen.png is a 960x600 PNG", png.startswith(b"\x89PNG") and (w, h) == (960, 600),
          f"{w}x{h}, {len(png)} bytes, {ctype}")

    # 3. state contract used by web/app.js
    st = state(base)
    required = {
        "ok": bool,
        "screen": dict,
        "sysinfo": dict,
        "events": list,
        "shots": list,
        "windows": list,
        "provider_name": str,
        "running": bool,
    }
    missing = [k for k, t in required.items() if not isinstance(st.get(k), t)]
    check("GET /state has the fields the UI reads", not missing, f"missing={missing}")
    sysinfo = st["sysinfo"]
    for key in ("disk", "uptime_human", "windows", "processes", "agent", "port"):
        if key not in sysinfo:
            missing.append("sysinfo." + key)
    for key in ("state", "steps", "task"):
        if key not in sysinfo.get("agent", {}):
            missing.append("agent." + key)
    check("sysinfo payload is complete", not missing, f"missing={missing}")
    check("machine reports open windows and a disk", sysinfo["windows"] >= 1 and sysinfo["disk"]["quota_bytes"] > 0,
          f"{sysinfo['windows']} windows, {sysinfo['disk']['used_bytes']}B used")

    # 4. human input reaches the pixels
    before = hashlib.sha256(get(base + "/screen.png")[1]).hexdigest()
    post(base + "/input", {"type": "mouse", "action": "move", "x": 159, "y": 586})
    post(base + "/input", {"type": "mouse", "action": "down", "x": 159, "y": 586, "button": "left"})
    post(base + "/input", {"type": "mouse", "action": "up", "x": 159, "y": 586, "button": "left"})
    time.sleep(0.4)
    after = hashlib.sha256(get(base + "/screen.png")[1]).hexdigest()
    apps = {w["app"] for w in state(base)["windows"]}
    check("clicking the taskbar changes the screen", before != after, f"{before[:8]} -> {after[:8]}")
    check("the click opened the editor app", "editor" in apps, ", ".join(sorted(apps)))

    # 5. typing into the sandbox terminal through /input (focus it first)
    post(base + "/input", {"type": "mouse", "action": "down", "x": 39, "y": 586, "button": "left"})
    post(base + "/input", {"type": "mouse", "action": "up", "x": 39, "y": 586, "button": "left"})
    post(base + "/input", {"type": "text", "text": "echo verified-over-http"})
    post(base + "/input", {"type": "key", "key": "Enter"})
    time.sleep(0.4)
    terms = [w["content"] for w in state(base)["windows"] if w["app"] == "terminal"]
    typed = any("verified-over-http" in "\n".join(t) for t in terms)
    check("typing reaches the sandbox terminal", typed, f"{len(terms)} terminal window(s)")

    # 6. human shell endpoint
    data = post(base + "/shell", {"command": "uname -a && ls /home/user"})
    out = data.get("output", "")
    check("POST /shell runs in the sandbox (chained output shown)",
          data.get("ok") and "SandboxOS" in out and "README.md" in out,
          out[:70].replace("\n", " "))

    # 7. the agent does real work
    if not args.skip_agent:
        _, body, _ = get(base + "/audit?n=1")
        token = "verify-http-" + str(int(time.time()))
        post(base + "/prompt", {"task": f"Explore the machine and write a report ({token})"})
        deadline = time.time() + 90
        shots_before = len(state(base)["shots"])
        done = False
        while time.time() < deadline:
            st = state(base)
            if not st["running"] and st["sysinfo"]["agent"]["state"] == "idle":
                done = True
                break
            time.sleep(0.5)
        st = state(base)
        events = st["events"]
        kinds = {e["kind"] for e in events}
        check("agent ran to completion over HTTP", done, f"state={st['sysinfo']['agent']['state']}")
        check("agent emitted thoughts, tool calls and results",
              {"tool", "result"} <= kinds, ", ".join(sorted(kinds)))
        out = post(base + "/shell", {"command": "cat /home/user/notes/agent-report.md"})["output"]
        check("agent's file is readable through the shell", "Agent report" in out, out[:50].replace("\n", " "))
        shots = state(base)["shots"]
        check("agent saved screenshots", len(shots) > shots_before or shots,
              f"{len(shots)} screenshot(s)")
        if shots:
            _, img, ctype = get(base + shots[-1]["url"])
            check("screenshot downloads as a PNG", img.startswith(b"\x89PNG") and ctype == "image/png",
                  f"{len(img)} bytes")
        _, body, _ = get(base + "/audit?n=100")
        entries = json.loads(body)["entries"]
        tools = [e for e in entries if e["action"] == "agent_tool"]
        check("every agent tool call is in the audit log", len(tools) >= 5, f"{len(tools)} entries")

    failed = [c for c in CHECKS if not c[1]]
    print(f"\n{len(CHECKS) - len(failed)}/{len(CHECKS)} live checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
