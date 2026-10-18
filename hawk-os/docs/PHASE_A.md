# Phase A — Reliability and Safety Foundation

Phase A hardens the existing execution loop before Hawk adds more execution engines.

## Included in this phase

- **Bounded direct scripts:** direct script runs use the configured step budget. A script that exceeds the budget stops with a visible failure instead of continuing indefinitely.
- **Bounded waits:** script delay, `wait`, `waitfocus`, and `waitclose` durations are capped at 30 seconds. Negative configured delays use a safe fallback where applicable.
- **Safer dry-run logs:** dry-run `type` actions log only the character count; typed text is not echoed into terminal or activity output.
- **Atomic progress checkpoints:** real script runs update `run-checkpoint.json` with status, attempted-step count, script offset, and timestamp. The manifest intentionally excludes command arguments and content, and dry runs do not write it.
- **Isolated goal-loop benchmark:** the mock model server binds to an ephemeral loopback port, avoiding fixed-port collisions, and always shuts down and cleans its fixture files.
- **New Windows CI regressions:** checks typed-content redaction, the direct-script step budget, and wait-duration caps in addition to the existing security and goal-loop suites.

## Run the checks on Windows

Build Hawk first, then run from `hawk-os/`:

```powershell
python tests/test_security.py
python tools/verify_goal_loop.py
python tools/verify_phase_a.py
```

The existing `hawk.exe --selftest` is interactive and touches the real desktop; run it only when you can supervise the machine.

## Acceptance criteria

- No secret sentinel appears in dry-run output for a `type` command.
- Direct scripts stop at their configured command limit and return failure when more commands remain.
- Excessive wait values are capped instead of allowing a long or effectively unbounded pause.
- Goal-loop tests use an isolated local mock server and clean up fixture files on success or failure.
- A successful model response alone is not proof of a task result; the goal loop must still use fresh observations and report execution failures.
- Checkpoint updates are atomic and contain no task text or action arguments.

## Not claimed as complete

This phase does not make Hawk a sandbox, guarantee correctness for every application, or automatically resume a task after a crash. The checkpoint is diagnostic progress metadata, not a replayable script. Native UI actions and ordinary mouse/keyboard control still affect the signed-in Windows session. Build/runtime checks and manual app testing remain necessary before release.
