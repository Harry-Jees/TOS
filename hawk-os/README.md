# Hawk OS

A desktop application, written in C++17 against the Win32 API, that **sees your
real screen and physically drives your real mouse and keyboard**.

It is not a simulation and not a virtual desktop: when it types, the characters
land in whatever window has focus; when it clicks, the real cursor moves and a
real button goes down. It can screenshot everything it does, so you can see
exactly what happened afterwards.

```
hawk.exe                      the app window (task-focused control center)
hawk.exe --selftest           prove capture + physical input work on this PC
hawk.exe --run "..."          execute agent commands
hawk.exe --goal "..."         ask a vision model to plan (plan-only by default)
hawk.exe --auto-run --goal "..."  explicitly allow the model plan to execute
hawk.exe --help               the full CLI
```

**Build status for this branch:** not yet verified on the maintainer's target laptop. Build and run the checks below before relying on it; hosted CI covers compilation and non-interactive regressions, not every real desktop/app interaction.

---

## 1. What it can do

Everything below is a real action on the real machine:

| Area | Commands |
| --- | --- |
| **See** | `shot`, `shotwin <title>`, `region x y w h`, `look`, `elements [x y w h]`, `windows`, `monitors`, `cursor`, `pixel x y` |
| **Touch** | `move x y`, `move rel dx dy`, `click [left\|right\|double] [x y]`, `clickel <rank> [right\|double]`, `drag x1 y1 x2 y2`, `scroll <notches>` |
| **Semantic controls** | `uia invoke "Save"`, `uia set "File name" "notes.txt"`, `uia toggle "Remember me"`, `uia select "Downloads"`, `uia expand "This PC"`, `uia focus "Search"`, `uia wait "Save" 5000` |
| **Aim at a screenshot** | `smove x y`, `sclick [btn] x y`, `sdrag x1 y1 x2 y2` - coordinates read off the last screenshot; Hawk maps them onto the real screen itself |
| **Type** | `type <text>` (real Unicode keystrokes, `\n` is Enter), `key ctrl+s`, `key alt+f4`, `key enter` |
| **Windows** | `focus <title part>`, `waitfocus <title part> [ms]`, `close <title part>`, `waitclose <title part> [ms]`, `open notepad`, `open https://example.com` |
| **Files** | `write <path> <text>`, `read <path>`, `mkdir <path>`, `ensure <path> [text]`, `del <path>` (file commands are confined to `workspace/`; absolute paths, `~`, and traversal are rejected) |
| **Flow** | `wait <ms>`, `try <cmd>`, `verifypixel x y RRGGBB [tol]`, `log <text>`, `# comment`, `done` |
| **Reusable workflows** | `workflow save <name>` (latest generated script), `workflow list`, `workflow run <name>` |

`find` / `findarea` are what make "press the button" possible without knowing
any coordinates: they scan a fresh screenshot for a colour - `find` for a
pixel, `findarea` for a whole region (a button, not one pixel of it) - and can
move/click what they found.

**Reusable workflows.** After a goal produces a script, use `workflow save invoice-check` to save it under `workspace/workflows/`, `workflow list` to browse saved recipes, and a standalone `workflow run invoice-check` line to execute one. Workflow names are restricted to safe slugs; files are size-bounded, nesting is capped, and the normal per-action approval policy still applies when expanded commands execute. Saving is an explicit approved action and does not overwrite an existing workflow. Review generated steps before saving; workflow storage is not a sandbox.

**Workflow preflight.** Before running a saved workflow, use the standalone command `workflow inspect invoice-check`. Hawk expands nested workflows, validates the branch/variable structure, and reports command count plus potentially risky command classes (for example browser submission, text entry, deletion, and desktop interaction). It does not execute steps or print their argument values. This is a review aid—not a guarantee of safety—so inspect the actual script and approve sensitive actions when prompted.

**Prefer semantic UI Automation when available.** Hawk now exposes the selected target app's accessible control names and can use native Windows control patterns to invoke buttons, set editable values, toggle checkboxes, select items, expand/collapse trees, or focus controls. Use the exact accessible name shown by `look` or the goal planner. Ambiguous names and unsupported patterns fail rather than choosing an arbitrary control; vision-grounded mouse/keyboard actions remain the fallback for custom-drawn or inaccessible apps. Semantic actions force a fresh visual verification before the plan continues.

**Scripts can see.** `look` captures the screen, detects the controls on it
(buttons, fields, panels) entirely locally - no model involved - and logs a
plain-text description with clickable coordinates. `elements [x y w h]` does
the same for a region, ranked by how control-like each one looks, and
`clickel 2` clicks the second-ranked control. `sclick 320 480` takes a point
read straight off the last screenshot and clicks the real screen there: Hawk
doing the coordinate arithmetic is one less thing to get wrong. So a script
like this works without any hardcoded pixel:

```
look            # what is on the screen right now?
sclick 320 480  # click the button at 320,480 of that screenshot
findarea FF840A 60 click   # or: click the biggest blue region on screen
```

## NVIDIA model configuration

Hawk requires a `.env` file in the same folder as `hawk.exe`. Copy `.env.example` to `.env`, add your NVIDIA API key, and set the model ID you want to use:

```dotenv
NVIDIA_API_KEY=nvapi-your-key-here
NVIDIA_BASE_URL=https://integrate.api.nvidia.com/v1
NVIDIA_MODEL=nvidia/nemotron-3.5-super-vl-120b-a12b
```

Get a key from NVIDIA's API catalog. Use a vision-capable model if you want screenshot-based planning. The model ID is sent with the OpenAI-compatible chat-completions request; Hawk does not download model weights to your PC. The API runs remotely through NVIDIA NIM.

**Keep `.env` private.** It is ignored by Git. Startup API credentials are read only from `NVIDIA_API_KEY` in `.env`; old keys in `hawk.ini`, process environment variables, and CLI key options are ignored. The Model Connection dialog changes only non-secret model preferences. See `docs/NVIDIA_SETUP.md`.

## 2. The app window

The refreshed control center is task-first and does **not** embed a screenshot preview:

- **Task brief** — describe what you want Hawk to accomplish, then run the goal.
- **Quick start** — load a click demo, Notepad demo, Hello Harry workflow, or command help.
- **Workspace** — edit the generated script and review a timestamped activity feed side by side.
- **Execution controls** — Run goal, Run script, STOP, Dry run, Restore cursor, and the explicitly confirmed Auto-run option.
- **NVIDIA setup** — view model preferences and the `.env` setup instructions; the API key is never entered in the UI.
- **Capture** — save a screenshot into `shots/` when requested. The app does not display that screenshot.
- **Status** — a clear Ready / Working / Stopping / Set up NVIDIA key indicator, with a subtle animated activity pulse while a job is running.

Hawk captures screen context only when a task needs it or when you press Capture; it does not continuously capture the desktop in the background. Vision goals still require consent before a screenshot is sent to NVIDIA NIM.

The **Model** button lets you adjust the non-secret model ID and endpoint. The API key is read exclusively from `.env` beside `hawk.exe`; old keys in `hawk.ini`, process environment variables, and CLI key options are ignored. Keep `.env` private and restart Hawk after changing the key.

## 3. Build

Zero configuration, no installer, no admin rights:

```bat
build.bat
```
```bash
./build.sh
```

Both prefer the portable toolchain in `../toolchains/zig` (Zig ships clang plus
the Windows headers and libraries) and fall back to `g++`/`clang++` on PATH if
that is missing. Any C++17 compiler that can link the Win32 import libraries
works:

```
-luser32 -lgdi32 -lshell32 -lwinhttp -lmsimg32 -lcomctl32 -ldwmapi -lole32 -lwindowscodecs -lcrypt32 -luiautomationcore -loleaut32
```

The executable is a GUI-subsystem binary, so no console window flashes when you
launch it; when you run it *from* a terminal it attaches to that console and
prints normally.

## 4. Safety model

Input injection is a sharp tool, so the guards are built in rather than
bolted on:

* **STOP** aborts between actions. An in-flight model request has bounded
  WinHTTP timeouts and may take up to 30 seconds to return before the worker
  can fully stop.
* **Ctrl+Alt+H** is a global hotkey that aborts even if the window is not
  focused. **Holding ESC** aborts too — checked before every action.
* A click that lands on a background window brings it to the front first —
  the attention a person pays before they click — so multi-window flows don't
  lose clicks to Windows' activation-click behaviour.
* **Dry run** executes the whole script with every action logged instead of
  performed, so you can read what it *would* have done first.
* **Model execution is opt-in.** Auto-run starts disabled. Review the generated
  plan before enabling it. The CLI requires `--auto-run` to execute a model
  plan automatically; `--plan-only` explicitly keeps execution disabled.
* **Screen sharing is disclosed.** Goal mode asks before sending a screenshot
  to the configured model provider. Cancel if the screen contains information
  you do not want to share.
* **Workspace writes are bounded.** File commands reject absolute/profile paths
  and traversal, and `write` refuses to overwrite an existing file. Delete
  it deliberately first if that is genuinely intended.
* **Typed content is redacted in logs.** The action log records the character
  count rather than echoing what was typed.
* **Restore cursor** (on by default) puts the mouse back where it started when
  a run ends.
* Every action is appended to `hawk.log` with a timestamp, and `--dry-run`
  writes nothing and touches nothing.
* Every step is verified where it can be: `move` reads the cursor position back
  and falls back to `SetCursorPos` if the normalised injection was refused,
  `click` reports where the cursor actually was, `find` re-probes the pixel it
  found, and a failed step stops the run instead of ploughing on.
* **Execution is bounded:** direct scripts stop at the configured step budget,
  and waits are capped at 30 seconds. Dry-run typing logs the character count,
  never the typed content. Real script runs atomically update `run-checkpoint.json`
  with privacy-safe progress metadata (not a resumable script). See [Phase A
  acceptance criteria](docs/PHASE_A.md).

What it does **not** do: it does not ask for elevation, install anything, or
touch the secure desktop. These controls are defense-in-depth, not a sandbox:
permitted mouse and keyboard actions can still affect signed-in apps and
destructive UI controls. Do not run untrusted tasks unattended.

## 5. Verification status and checks

Do not treat old screenshots, historical test counts, or a successful compile as proof that the current branch is safe on every desktop. The current branch's checks are intended to cover:

- `python tests/test_security.py` — workspace traversal, overwrite protection, safe file creation, and sensitive-action defaults.
- `python tools/verify_goal_loop.py` — offline mocked-model plan/execute/verify scenarios, including screenshot attachment and Unicode output.
- `hawk.exe --selftest` — interactive Win32 capture/input primitives on the actual desktop.
- Manual checks — UI startup, settings/key migration, approval prompts, STOP behavior, screen-consent flow, and apps you intend to automate.

Build on Windows with the supported toolchain, then run the two non-interactive checks from `hawk-os/`. The self-test and manual checks touch the real desktop, so run them only in a session you control and save your work first. **This branch has not been validated on your laptop yet.**

## 6. Limitations, honestly

* **Elevated windows are out of reach.** Windows UIPI stops a normal process
  injecting input into an admin window, and nothing can touch the UAC secure
  desktop. Hawk should not be run elevated by default; elevated automation is
  intentionally outside the normal Phase 1 scope.
* **Some games ignore synthetic input** because they read raw devices.
* **The human and the agent share one mouse.** If you move the physical mouse
  mid-run, Windows resynchronises the cursor and the agent's position is gone.
  The logger reports the drift, and dry run plus STOP are the answer. This is
  why checks that would otherwise be pixel-exact are asserted against the
  position the OS reported at injection time, not a moment later.
* **Vision planning costs money and can be wrong.** The model sees a downscaled
  still image plus Hawk's own local analysis of it; `verifypixel`, `find`,
  `findarea` and `look` exist so a script can check its own work instead of
  trusting the plan. A key without inference access is reported exactly as it
  is (HTTP 403) - Hawk never falls back to guessing a plan.
* **Nothing is code-signed**, so SmartScreen will warn the first time. There is
  no auto-update and no telemetry.
* **PNG fallback is uncompressed.** Without WIC you get valid but large files.

## 7. Layout

```
hawk-os/
  hawk.exe            built binary (static, GUI subsystem)
  hawk.ini            settings written by the Model dialog (not in git)
  hawk.log            every action, timestamped
  build.sh build.bat  builds with the bundled toolchain or a system compiler
  ui-layout.txt       the app's own window/control geometry in screen pixels
  src/hawk.h          shared types, module boundaries
  src/util.cpp        strings, paths, console attach
  src/image.cpp       image buffers, scaling, colour search, PNG/BMP, base64
  src/capture.cpp     screen, region, per-window capture; DPI awareness
  src/input.cpp       SendInput mouse/keyboard, cursor, wheel, abort checks
  src/winctl.cpp      window enumeration, lookup, focus
  src/agent.cpp       the command language, the step loop, the model client
  src/uia.cpp         Windows UI Automation accessibility-tree perception
  src/ui.cpp          the task control center, owner-drawn controls, worker thread
  src/main.cpp        entry point, CLI, self-test
  tests/              non-interactive safety regression tests and other test helpers
  tools/              verify_goal_loop.py (offline goal-loop checks) + probes
  examples/           ready-to-run command scripts
  shots/              captures (git-ignored)
  workspace/          files the agent writes with relative paths
```


## 6. Open-source development and future direction

Hawk is being developed as an open-source desktop agent, not an operating
system. The target experience is a coworker-style task workflow: inspectable
plans, step-by-step progress, approvals for risky actions, recoverable runs,
and verified outcomes.

- [Architecture and hardening roadmap](docs/ARCHITECTURE.md)
- [Android voice companion design](docs/MOBILE_COMPANION.md)
- [Open-source roadmap](OPEN_SOURCE_ROADMAP.md)
- [Security policy](../SECURITY.md)
- [Contributing](../CONTRIBUTING.md)

A future Android companion will submit voice instructions and display progress
on a paired PC. The Windows machine will remain the execution authority; the
mobile app must not bypass local approval or send provider API keys to the
phone or relay.

The current build remains a prototype. The updated code has not yet been
validated by a Windows build in this development session; follow the CI status
and release notes before relying on it.
