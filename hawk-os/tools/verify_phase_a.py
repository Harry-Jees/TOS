"""Offline Phase A regressions for bounded execution and safe logging.

Run after building Hawk on Windows:
    python tools/verify_phase_a.py
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "hawk.exe")


def run(*args):
    return subprocess.run(
        [EXE, *args], cwd=ROOT, capture_output=True, text=True,
        timeout=30, errors="replace"
    )


def main():
    if not os.path.isfile(EXE):
        print("FAIL: hawk.exe not found; build Hawk first")
        return 2

    failures = 0

    # Dry-run output must never echo text that would normally be typed. This
    # catches accidental leakage into the activity log or captured terminal.
    secret = "HAWK_PHASE_A_PRIVATE_SENTINEL_9D3F"
    r = run("--dry-run", "--run", f'type "{secret}"')
    output = r.stdout + r.stderr
    ok = r.returncode == 0 and secret not in output and "content redacted" in output
    print(f"dry-run typed content is redacted: {'PASS' if ok else 'FAIL'}")
    failures += not ok

    # Direct scripts need a hard upper bound too; otherwise a script file can
    # run indefinitely even though goal-mode has its own step budget.
    r = run("--dry-run", "--steps", "2", "--run",
            "log phase-a-one\nlog phase-a-two\nlog phase-a-three")
    output = r.stdout + r.stderr
    ok = r.returncode != 0 and "command limit reached (2)" in output
    print(f"direct script step budget enforced: {'PASS' if ok else 'FAIL'}")
    failures += not ok

    # User/model-supplied waits are capped before execution, including dry-run
    # output, so a typo cannot request an effectively unbounded wait.
    r = run("--dry-run", "--run", "wait 99999999")
    output = r.stdout + r.stderr
    ok = r.returncode == 0 and "waiting 30000 ms" in output
    print(f"wait duration is bounded: {'PASS' if ok else 'FAIL'}")
    failures += not ok

    # Checkpoints must be atomic, useful after a crash, and free of command
    # arguments. Preserve any previous manifest so the test does not destroy
    # the maintainer's diagnostic state.
    checkpoint = os.path.join(ROOT, "run-checkpoint.json")
    previous = None
    if os.path.isfile(checkpoint):
        with open(checkpoint, "rb") as f:
            previous = f.read()
    try:
        r = run("--run", "log phase-a-checkpoint-probe")
        try:
            with open(checkpoint, "r", encoding="utf-8") as f:
                manifest = json.load(f)
            checkpoint_ok = (
                r.returncode == 0
                and manifest.get("schema_version") == 1
                and manifest.get("status") == "completed"
                and manifest.get("attempted_steps") == 1
                and "phase-a-checkpoint-probe" not in json.dumps(manifest)
            )
        except (OSError, ValueError):
            checkpoint_ok = False
        print(f"privacy-safe run checkpoint persisted: {'PASS' if checkpoint_ok else 'FAIL'}")
        failures += not checkpoint_ok
    finally:
        if previous is None:
            try:
                os.remove(checkpoint)
            except FileNotFoundError:
                pass
        else:
            with open(checkpoint, "wb") as f:
                f.write(previous)

    print(f"\n{4 - failures}/4 Phase A checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
