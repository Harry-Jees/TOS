# Hawk OS

A desktop application, written in C++17 against the Win32 API, that **sees your
real screen and physically drives your real mouse and keyboard**.

It is not a simulation and not a virtual desktop: when it types, the characters
land in whatever window has focus; when it clicks, the real cursor moves and a
real button goes down. It can screenshot everything it does, so you can see
exactly what happened afterwards.

```
hawk.exe                      the app window (live screen + Agent panel)
hawk.exe --selftest           prove capture + physical input work on this PC
hawk.exe --run "..."          execute agent commands
hawk.exe --goal "..."         let a vision model plan and act
hawk.exe --help               the full CLI
```

Built and verified on Windows 11, 1600x900, 32-bit colour, no third-party
libraries and no runtime dependencies (a single ~630 KB static `hawk.exe`).

---

## 1. What it can do

Everything below is a real action on the real machine:

| Area | Commands |
| --- | --- |
| **See** | `shot`, `shotwin <title>`, `region x y w h`, `look`, `elements [x y w h]`, `windows`, `monitors`, `cursor`, `pixel x y` |
| **Touch** | `move x y`, `move rel dx dy`, `click [left\|right\|double] [x y]`, `clickel <rank> [right\|double]`, `drag x1 y1 x2 y2`, `scroll <notches>` |
| **Aim at a screenshot** | `smove x y`, `sclick [btn] x y`, `sdrag x1 y1 x2 y2` - coordinates read off the last screenshot; Hawk maps them onto the real screen itself |
| **Type** | `type <text>` (real Unicode keystrokes, `\n` is Enter), `key ctrl+s`, `key alt+f4`, `key enter` |
| **Windows** | `focus <title part>`, `waitfocus <title part> [ms]`, `close <title part>`, `waitclose <title part> [ms]`, `open notepad`, `open https://example.com` |
| **Files** | `write <path> <text>`, `read <path>`, `mkdir <path>`, `ensure <path> [text]`, `del <path>` (file commands are confined to `workspace/`; absolute paths, `~`, and traversal are rejected) |
| **Flow** | `wait <ms>`, `try <cmd>`, `verifypixel x y RRGGBB [tol]`, `log <text>`, `# comment`, `done` |

`find` / `findarea` are what make "press the button" possible without knowing
any coordinates: they scan a fresh screenshot for a colour - `find` for a
pixel, `findarea` for a whole region (a button, not one pixel of it) - and can
move/click what they found.

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

## 2. The app window

One dark window, no menus to hunt through:

* **Left** — a live, scaled view of your screen, refreshed ~6 times a second,
  with a green crosshair on the real cursor and a red ring on the last click it
  made. Tick **Click-through** and clicking the preview really clicks your
  screen at that point (with self-click suppression so it cannot loop).
* **Right** — the **Agent**: a goal box, a script box, Run goal / Run script /
  **STOP**, Capture, Shots folder, Model settings, and an activity log with
  timestamps and level tags.
* A **run-state pill** in the header (IDLE / RUNNING / AGENT RUNNING /
  STOPPING) and a **DRY RUN** pill whenever nothing will be touched, so the
  mode of the next action is readable across the room.
* Quick actions load working examples: *Click middle*, *Notepad demo*, *Help*.
* The window publishes its own geometry to `ui-layout.txt`, which is how a test
  (or another Hawk OS run) can click the app's buttons by coordinate.

**Model settings** (the *Model* button) stores a base URL, model id, and a
Windows DPAPI-protected API key in `hawk.ini` next to the executable. Legacy
plaintext keys are still read for migration; saving settings rewrites them in
protected form. The key is never intentionally printed to the log, screen, or
terminal. Any OpenAI-compatible endpoint works — NVIDIA
NIM is the default (`https://integrate.api.nvidia.com/v1`) with a vision model
that can actually read screenshots (`nvidia/nemotron-3.5-super-vl-120b-a12b`,
NVIDIA's first-party vision-language model). Or set `HAWK_API_KEY`,
`HAWK_BASE_URL`, `HAWK_MODEL`, or pass `--key-file <path>`.

**Run goal** is built for multi-step tasks, not one-shot clicks — "open my
editor, write this code, save it and open it again" is the shape of task it is
for. It works in three phases, all grounded in fresh screenshots:

1. **Plan** — the goal, a fresh screenshot (downscaled, overlaid with a labelled
   coordinate grid and the mouse position), a local analysis of the controls
   detected on that screenshot, and the window list go to the model, which
   writes the *complete* script for the whole task.
2. **Execute** — Hawk runs the script in segments through its own verified step
   engine. Each command's real result (ok or fail) is recorded; a failed step
   stops the segment instead of ploughing on.
3. **Verify** — after each segment (and after any failure) a new screenshot plus
   the execution transcript goes back to the model. It answers `done` (only a
   later screenshot may declare that), `continue`, or a small batch of
   corrective commands that are inserted in front of the remaining plan.

The generated script is always shown in the script box and saved to
`workspace/`, so "the agent wrote a script and ran it" leaves something you can
re-run. If a model request fails, it reports the error rather than silently
substituting a guessed script. A model key is required for screenshot-based
planning; without one, supported simple goals can still use the built-in
planner, and existing scripts can still be run directly.

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
-luser32 -lgdi32 -lshell32 -lwinhttp -lmsimg32 -lcomctl32 -ldwmapi -lole32 -lwindowscodecs
```

The executable is a GUI-subsystem binary, so no console window flashes when you
launch it; when you run it *from* a terminal it attaches to that console and
prints normally.

## 4. Safety model

Input injection is a sharp tool, so the guards are built in rather than
bolted on:

* **STOP** button aborts the run between steps, instantly.
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

What it does **not** do: it does not ask for elevation, install anything, or
touch the secure desktop. These controls are defense-in-depth, not a sandbox:
permitted mouse and keyboard actions can still affect signed-in apps and
destructive UI controls. Do not run untrusted tasks unattended.

## 5. How this was verified

Not "it compiled". These are the checks that ran against the built binary:

* **`hawk.exe --selftest` — 21/21.** It builds a real window with a real target
  square, then uses its own input engine to move the cursor there and click:
  the click arrives as a `WM_LBUTTONDOWN` at client `85,125`, inside the
  target. It also types 16 characters into a live `EDIT` control (all 16 arrive
  in order), captures the screen, writes and re-reads a PNG, finds a colour it
  painted, and proves a dry-run script executes nothing. The vision
  primitives get the same treatment: `findarea` has to locate the whole blue
  square it painted, `detect_elements` has to rank that control, and
  `draw_grid` has to visibly overlay its grid.
* **`python tests/test_hawk.py` — 19/19.** Drives the CLI and inspects the
  artefacts: PNGs are fully decoded in Python (all five filter types, every
  CRC checked), the pixel the tool reports is compared against the decoded
  screenshot, `find` is verified by probing the location it returned, the
  fallback encoder is exercised with `HAWK_NO_WIC=1`, and goal-mode checks
  confirm screenshot attachment, vision requirements, and no guessed-plan
  fallback when a model request fails. The pixel-comparison checks sample a
  quiet corner of the taskbar and accept a screenshot taken immediately after
  the probe, so a desktop with a playing video or a live editor open cannot
  make them lie.
* **`python tools/verify_goal_loop.py` — 3/3.** Runs the plan → execute →
  verify loop against a local mock model, no API key needed: a plan is written,
  executed and verified `done`; a corrective batch from a verify round is
  inserted and executed; a plan that ends in `done` is verified with a fresh
  screenshot instead of believed. Every vision request is asserted to really
  carry the screenshot.
* **`python tests/test_gui.py` — 12/12.** Starts the app, checks the geometry
  it publishes, and confirms from captured pixels that the accent Run button,
  the red STOP button, the header gradient and the screen preview really
  rendered. Then it clicks the app's own **Capture** button *with Hawk OS's own
  input engine* and asserts a new screenshot appeared on disk.
* **`python tests/test_real_app.py` — 5/5.** Opens Notepad (a real third-party
  application), opens a fresh tab, types 55 characters with synthetic
  keystrokes, presses Save As (Ctrl+Shift+S), types a filename into Notepad's
  own Save dialog, presses Enter — and the test then reads
  `~/Documents/hawk-agent-proof.txt` from disk and checks the text matches byte
  for byte.

Screenshots are compressed by Windows Imaging Component (a 1600x900 capture is
~150-300 KB instead of 5.7 MB raw), which matters because goal mode has to send
them to a model; the hand-written uncompressed encoder remains as a fallback
that needs nothing from the system.

`shots/` collects everything captured; the `--goal` path keeps its own shots
prefixed `goal`.

## 6. Limitations, honestly

* **Elevated windows are out of reach.** Windows UIPI stops a normal process
  injecting input into an admin window, and nothing can touch the UAC secure
  desktop. Run Hawk OS as administrator if you need that (and accept the risk).
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
  src/ui.cpp          the app window, preview, ownerdraw controls, worker thread
  src/main.cpp        entry point, CLI, self-test
  tests/              the three suites described above
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
