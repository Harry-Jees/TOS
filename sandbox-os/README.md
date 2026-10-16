# SandboxOS

A small operating system written in Python — its own filesystem, its own shell, its own
screen with a window manager — with an **AI agent living inside it** that can do anything
in that machine: read and write files, run commands, open windows, click buttons, type
text and take screenshots of what it sees.

The sandbox is the point: the agent has hands, but those hands can only reach *inside*
SandboxOS. There are no host binaries, no host filesystem and no network in the sandbox,
so "doing anything in it" cannot become "doing anything to your PC".

```
desktop.bat                # Windows: launch the desktop app (native window)
python run.py --desktop    # the same app, from a terminal
python run.py              # or: boot a machine and serve the browser console instead
```

Windows: double-click **`desktop.bat`** for the desktop app, or `start.bat` for the browser
console. Requires only Python 3.10+ with tkinter (bundled with the Windows installer) — no
pip installs; the PNG encoder and the bitmap font are standard library too.

---

## 1. What you get

| Part | What it is |
|---|---|
| **Virtual disk** | A jailed `root/` directory presented as a POSIX tree (`/home/user`, `/etc`, `/var/log`, `/tmp`) with byte quotas, an inode budget and a per-file size cap. |
| **Shell** | A real interpreter: `ls, cat, echo, mkdir, rm, cp, mv, grep, find, tree, head, tail, wc, df, du, ps, uptime, neofetch ...` with quoting, globs, pipes `|`, redirection `> >>`, and `; && \|\|`. No `python`, `bash`, `curl`, `cmd`, `powershell` — they do not exist here. |
| **Screen** | A 960×600 RGB framebuffer painted by a window manager: top panel, taskbar, draggable windows, close buttons, scrollbars, mouse cursor. Apps: `terminal`, `files`, `editor`, `sysmon`, `agent`. |
| **AI agent** | A tool-calling loop with 22 tools over that disk, shell and screen. Works with any OpenAI-compatible endpoint (NVIDIA NIM, OpenAI, OpenRouter, Groq, local Ollama) or with the built-in offline scripted brain. |
| **Desktop app** | A native Tk window — the whole machine in one app: live screen, agent feed, system readout, screenshot gallery, audit trail, task box and a human shell, with menus for self-test, factory reset and your model key. No browser required. |
| **Console** (optional) | The same thing as a local web UI on `http://127.0.0.1:8787`, if you prefer a browser tab or want to watch from another device on a tunnel. You and the agent share the same screen either way — click or type and the machine reacts. |
| **Audit log** | Every action (yours and the agent's) appended to `var/audit.log` as JSONL, with secrets redacted. |

## 2. The desktop app

`desktop.bat` or `python run.py --desktop` opens one window containing the whole machine:

| Area | What it does |
|---|---|
| **Live screen** | The 960×600 virtual framebuffer. It is redrawn **only when the kernel's frame actually changes**, straight from a cached PNG — no polling of the host, no flicker. Click it to focus, and your real mouse moves the sandbox cursor, your real clicks press sandbox buttons, your keyboard types into the focused sandbox window. Drag a title bar to move a sandbox window. |
| **Task box** | Type a task, press Run or Enter. Abort stops the agent mid-run. |
| **Agent tab** | The live feed, colour-coded: thoughts, tool calls, results, errors, screenshots, summaries. |
| **System tab** | Uptime, windows, processes, files, disk usage with a quota bar, agent state. |
| **Screenshots tab** | Thumbnails of what the agent captured; click one to open the full PNG. |
| **Audit tab** | The host-side audit trail: actor, action, detail. |
| **shell>** | Run a shell command in the sandbox yourself; output lands in the feed. |
| **Machine menu** | *Set NVIDIA / OpenAI-compatible API key…* (writes to `.env`, with a **Test connection** button), *Reload model settings*, *Self-test*, *Factory reset* (wipe the sandbox disk and boot fresh), *Quit*. |
| **Windows / View menus** | Open or close the sandbox apps; switch the screen between 1:1 and 50%; toggle feed auto-scroll. |
| **Help menu** | About/status (which key source is active, model, base URL, all paths) and shortcuts to open the sandbox disk, audit log or screenshots folder. |

Scale is 1:1, dropping to 50% automatically on small displays (`--scale 0.5` to force it).

### The flicker fix

Both front ends render the screen the same careful way: the kernel caches one PNG per frame
(`WindowManager.screenshot_png()` keyed on `frame_seq`), the desktop app reuses the same
canvas image item, and the browser console keeps a single canvas bitmap — it only touches
`canvas.width/height` when the size genuinely changes, because assigning those *clears* the
canvas. That reassignment on every poll was what made the screen flicker, and it is gone in
both clients.

## 3. Add your model key

Copy `.env.example` to `.env` and set your NVIDIA key (from <https://build.nvidia.com>):

```ini
SANDBOX_LLM_API_KEY=nvapi-...
SANDBOX_LLM_BASE_URL=https://integrate.api.nvidia.com/v1
SANDBOX_LLM_MODEL=meta/llama-3.3-70b-instruct
```

Without a key the machine still boots and the agent still works — it just uses the
built-in **scripted brain** (a deterministic multi-step run that writes a report, drives
the terminal and takes screenshots), which is also what the test suite uses.

Notes for NVIDIA NIM: the endpoint is OpenAI-compatible; pick a model that supports tool
calling (`meta/llama-3.3-70b-instruct`, `nvidia/llama-3.3-nemotron-super-49b-v1.5`,
`qwen/qwen2.5-coder-32b-instruct`, `deepseek-ai/deepseek-v3.1`). A wrong model name comes
back as HTTP 404 with the message in the agent feed. If an endpoint rejects native tool
calling, the loop automatically switches to a JSON-action protocol instead.

## 4. What the agent can do

**Filesystem** — `fs_list`, `fs_read`, `fs_write`, `fs_mkdir`, `fs_delete`, `fs_move`,
`fs_stat`, `fs_search`
**Shell** — `shell_run` (pipes, redirection, `&&`/`||`, exit codes)
**Screen** — `screen_describe` (the screen as text: windows, rects, focus, visible content,
cursor), `screen_screenshot` (PNG for you to look at), `screen_move`, `screen_click`,
`screen_drag`, `screen_scroll`, `screen_type`, `screen_key`
**Windows** — `window_open`, `window_list`, `window_focus`, `window_close`
**Machine** — `sys_info`, `task_complete`

The agent has no vision by default, so it works from `screen_describe`: it reads the window
list, then clicks real coordinates, then reads the result back. That is why it can drive a
terminal window it cannot literally see.

## 5. What the sandbox guarantees (and what it does not)

Guaranteed, and enforced by the code:

* **Path jail.** Every path is normalised and verified to stay inside `root/`; `..`,
  drive-letter paths, UNC paths, reserved device names and symlinks that point outside are
  refused. `normalize()` raises rather than silently clamping, so nothing can *believe* it
  reached the host.
* **No host execution.** The shell is an interpreter, not a subprocess. `python`, `bash`,
  `cmd`, `powershell`, `curl` return `127 command not found`.
* **No network** from inside the sandbox; the model call happens in the host-side agent,
  never in the sandbox shell.
* **Resource limits.** 8 MiB disk quota, 1 MiB per file, 2000 entries, 20 000 chars of
  command output, plus an agent step budget and time budget.
* **Auditability.** `var/audit.log` records boots, shell commands, window opens, every
  agent tool call with arguments/result, and every task start/end.
* **Loopback only** console, with static files served from disk and screenshots served from
  a fixed directory (no traversal).

Not guaranteed — be honest about the boundary:

* This is **application-level** isolation, not a hypervisor. The Python process runs as
  your user; it is the *agent's tools* that are confined, not the OS from the process. For
  kernel-level isolation you would run this inside a container or VM (add a `Dockerfile`
  with Xvfb + this project as the next step).
* The console is protected by nothing but being bound to `127.0.0.1`. Do not expose the
  port; anyone who can reach it can drive the sandbox.
* The agent's screenshots are for **you**; a text model cannot read them. Wiring a vision
  model into `screen_screenshot` would be the natural upgrade.

## 6. Layout

```
desktop.bat             Windows launcher for the desktop app
start.bat               Windows launcher for the browser console
run.py                  launcher: --desktop, serve, --task, --headless, --self-test
desktop.py              the native tkinter application (window, tabs, menus, key dialog)
server.py               optional loopback HTTP console + control API
sandboxos/
  kernel.py             boot, event bus, sysinfo, shell/app control, audit, factory reset
  selftest.py           the boundary self-test, shared by CLI and desktop menus
  vfs.py                the jailed filesystem (quotas, normalisation, containment)
  shell.py              the shell interpreter and built-in commands
  screen.py             framebuffer, window manager, apps, synthetic input
  font.py               5x7 bitmap font (dependency-free text on the virtual screen)
  png.py                PNG encoder (dependency-free screenshots)
  audit.py              JSONL audit log with secret redaction
  config.py             paths, limits, .env loading
agentd/
  runner.py             the think -> act -> observe loop, budgets, abort, JSON fallback
  tools.py              the 22 tools + JSON schemas, all bound to the kernel
  providers.py          OpenAI-compatible HTTP client, scripted brain, provider selection
  prompts.py            system prompt / JSON protocol
web/                    the console UI (index.html, app.js, style.css)
tests/                  93 unit + integration tests (incl. the desktop app)
tools/verify_live.py    end-to-end check of a running console over HTTP
tools/capture_window.py grabs the app's real window and compares its canvas to the framebuffer
root/                   the sandbox disk (runtime data, safe to delete)
var/                    audit log and saved screenshots (host side, outside the jail)
```

## 7. Using it

Human side — in the **desktop app**: type a task and press Run; watch the feed; click the
screen to focus it and then use your own mouse and keyboard inside the sandbox (click the
taskbar to open apps, drag title bars, type into the terminal); click a screenshot thumbnail
to see exactly what the agent captured. In the **browser console** it is the same, with
quick-task buttons instead of menus.

CLI side:

```bash
python run.py --desktop                                      # the desktop app
python run.py --desktop --task "tidy /home/user"             # app with a task already running
python run.py --desktop --scale 0.5                          # smaller screen in the app
python run.py --task "tidy /home/user and write a summary"   # boot, serve, and start a task
python run.py --headless --task "..."                        # no server, print the summary
python run.py --self-test                                    # prove the boundaries hold
python -m unittest discover -s tests -t .                    # 93 tests
python tools/verify_live.py                                  # check a running console end to end
python tools/capture_window.py                               # grab the app window and verify it renders
python run.py --scripted                                     # force the offline brain
```

## 8. Extending it

* **A new agent tool**: add a `_schema(...)` entry to `TOOL_SCHEMAS` and a `t_<name>` method
  on `Toolbox` in `agentd/tools.py`. It appears in the model's tool list automatically.
* **A new app window**: subclass `Window` in `sandboxos/screen.py`, implement `draw_content`
  / `on_key` / `on_click` / `describe` (the last one is how the agent perceives it), then
  register it in `APP_CLASSES` and in `WindowManager.list_apps()`.
* **A different brain**: implement `chat(messages, tools)` in `agentd/providers.py`; return
  `{"content", "tool_calls": [{"id", "name", "arguments"}], "finish_reason"}`.
* **A different model endpoint**: change `SANDBOX_LLM_BASE_URL` / `SANDBOX_LLM_MODEL` in `.env`.

## 9. Troubleshooting

| Symptom | Fix |
|---|---|
| `ModuleNotFoundError: tkinter` | The app needs tkinter (bundled with the normal Windows/macOS installers; on Linux `sudo apt install python3-tk`). The browser console works without it: `python run.py`. |
| App window is too big for the screen | It starts at 50% scale automatically on small displays; use `--scale 0.5`. |
| Desktop app *and* browser console at once | They are two separate machines that share the same sandbox disk (`root/`) and audit log, so files written in one show up in the other, but their windows are independent. Run one or the other, or close one when you switch. |
| `could not bind a local port` | Something holds 8787–8806; pass `--port 9100`. |
| Agent says "model error: HTTP 401" | Key missing/invalid in `.env` (the value is never printed anywhere). |
| "HTTP 404" with a model name | That model id does not exist on your endpoint; pick another from the catalogue. |
| Endpoint rejects tools | Automatic: the loop falls back to the JSON-action protocol. |
| Feed says "switching to JSON protocol" on every step | Your model is weak at tool calling; try a 70B-class instruct model. |
| Text looks blocky | It is a hand-authored 5×7 bitmap font rendered into a 960×600 framebuffer, scaled up in the browser. |
