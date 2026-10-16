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
| **See** | `shot`, `shotwin <title>`, `region x y w h`, `windows`, `monitors`, `cursor`, `pixel x y`, `find RRGGBB [tol] [move\|click\|double]` |
| **Touch** | `move x y`, `move rel dx dy`, `click [left\|right\|double] [x y]`, `drag x1 y1 x2 y2`, `scroll <notches>` |
| **Type** | `type <text>` (real Unicode keystrokes, `\n` is Enter), `key ctrl+s`, `key alt+f4`, `key enter` |
| **Windows** | `focus <title part>`, `close <title part>`, `waitclose <title part> [ms]`, `open notepad`, `open https://example.com` |
| **Files** | `write <path> <text>`, `read <path>`, `mkdir <path>`, `ensure <path> [text]`, `del <path>` (relative paths land in `workspace/`; `~` starts at your profile folder) |
| **Flow** | `wait <ms>`, `try <cmd>`, `verifypixel x y RRGGBB [tol]`, `log <text>`, `# comment`, `done` |

`find` is the one that makes "press the button" possible without knowing any
coordinates: it scans a fresh screenshot for a colour and can move/click it
directly.

## 2. The app window

One dark window, no menus to hunt through:

* **Left** — a live, scaled view of your screen, refreshed ~6 times a second,
  with a green crosshair on the real cursor and a red ring on the last click it
  made. Tick **Click-through** and clicking the preview really clicks your
  screen at that point (with self-click suppression so it cannot loop).
* **Right** — the **Agent**: a goal box, a script box, Run goal / Run script /
  **STOP**, Capture, Shots folder, Model settings, and a colour-coded log
  (`ok` / `fail` / `action` / `agent`).
* Quick actions load working examples: *Click middle*, *Notepad demo*, *Help*.
* The window publishes its own geometry to `ui-layout.txt`, which is how a test
  (or another Hawk OS run) can click the app's buttons by coordinate.

**Model settings** (the *Model* button) stores an API key, a base URL and a
model id in `hawk.ini` next to the executable; the key is never printed to the
log, the screen or the terminal. Any OpenAI-compatible endpoint works — NVIDIA
NIM is the default (`https://integrate.api.nvidia.com/v1`,
`meta/llama-3.2-90b-vision-instruct`). Or set `HAWK_API_KEY`, `HAWK_BASE_URL`,
`HAWK_MODEL`, or pass `--key-file <path>`.

With a vision-capable model configured, **Run goal** sends the goal, a fresh
downscaled screenshot, the screenshot-to-desktop coordinate mapping, and the
window list. It executes at most three model commands, captures the resulting
screen, and asks the model what to do next. It only accepts `done` from a
later screenshot check; if a model request fails, it reports the error rather
than silently substituting a guessed script. A model key is required for
screenshot-based planning. Without a key, supported simple goals can still use
the built-in planner, and existing scripts can still be run directly.

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
* **Dry run** executes the whole script with every action logged instead of
  performed, so you can read what it *would* have done first.
* **Restore cursor** (on by default) puts the mouse back where it started when
  a run ends.
* Every action is appended to `hawk.log` with a timestamp, and `--dry-run`
  writes nothing and touches nothing.
* Every step is verified where it can be: `move` reads the cursor position back
  and falls back to `SetCursorPos` if the normalised injection was refused,
  `click` reports where the cursor actually was, `find` re-probes the pixel it
  found, and a failed step stops the run instead of ploughing on.

What it does **not** do: it does not ask for elevation, does not install
anything, does not touch a UAC prompt or the secure desktop (Windows blocks
synthetic input there), and it does not hide: the window shows what it is
doing, every time.

## 5. How this was verified

Not "it compiled". These are the checks that ran against the built binary:

* **`hawk.exe --selftest` — 18/18.** It builds a real window with a real target
  square, then uses its own input engine to move the cursor there and click:
  the click arrives as a `WM_LBUTTONDOWN` at client `85,125`, inside the
  target. It also types 16 characters into a live `EDIT` control (all 16 arrive
  in order), captures the screen, writes and re-reads a PNG, finds a colour it
  painted, and proves a dry-run script executes nothing.
* **`python tests/test_hawk.py` — 19/19.** Drives the CLI and inspects the
  artefacts: PNGs are fully decoded in Python (all five filter types, every
  CRC checked), the pixel the tool reports is compared against the decoded
  screenshot, `find` is verified by probing the location it returned, the
  fallback encoder is exercised with `HAWK_NO_WIC=1`, and goal-mode checks
  confirm screenshot attachment, vision requirements, and no guessed-plan
  fallback when a model request fails.
* **`python tests/test_gui.py` — 12/12.** Starts the app, checks the geometry
  it publishes, and confirms from captured pixels that the accent Run button,
  the red STOP button, the header gradient and the screen preview really
  rendered. Then it clicks the app's own **Capture** button *with Hawk OS's own
  input engine* and asserts a new screenshot appeared on disk.
* **`python tests/test_real_app.py` — 5/5.** Opens Notepad (a real third-party
  application), types 55 characters with synthetic keystrokes, presses Ctrl+S,
  types a filename into Notepad's own Save dialog, presses Enter — and the test
  then reads `~/Documents/hawk-agent-proof.txt` from disk and checks the text
  matches byte for byte.

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
* **Vision planning costs money and can be wrong.** The model only sees a
  downscaled still image; `verifypixel` and `find` exist so a script can check
  its own work instead of trusting the plan.
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
  examples/           ready-to-run command scripts
  shots/              captures (git-ignored)
  workspace/          files the agent writes with relative paths
```
