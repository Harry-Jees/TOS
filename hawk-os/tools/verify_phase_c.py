"""Phase C regressions for safe resumable workflow checkpoints (Windows)."""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "hawk.exe")
WORKFLOWS = os.path.join(ROOT, "workspace", "workflows")


def run(*args):
    return subprocess.run([EXE, *args], cwd=ROOT, capture_output=True,
                          text=True, timeout=30, errors="replace")


def main():
    if not os.path.isfile(EXE):
        print("FAIL: hawk.exe not found; build Hawk first")
        return 2

    os.makedirs(WORKFLOWS, exist_ok=True)
    name = f"phase-c-resume-{os.getpid()}"
    path = os.path.join(WORKFLOWS, name + ".hawk")
    checkpoint = os.path.join(ROOT, "run-checkpoint.json")
    outcomes = os.path.join(ROOT, "workspace", "run-outcomes.jsonl")
    backups = {}
    for p in (path, checkpoint, outcomes):
        if os.path.isfile(p):
            with open(p, "rb") as f:
                backups[p] = f.read()
    failures = 0
    original = "log PHASE_C_FIRST\nlog PHASE_C_LAST\n"
    try:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(original)

        r = run("--steps", "1", "--run", f"workflow run {name}")
        try:
            with open(checkpoint, "r", encoding="utf-8") as f:
                manifest = json.load(f)
            ok = (r.returncode != 0 and manifest.get("status") == "limit_reached"
                  and manifest.get("workflow_name") == name
                  and bool(manifest.get("script_hash"))
                  and manifest.get("next_script_offset") == len("log PHASE_C_FIRST\n"))
        except (OSError, ValueError):
            ok = False
        print(f"checkpoint records a resumable workflow boundary: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        r = run("--run", "task status")
        output = r.stdout + r.stderr
        ok = (r.returncode == 0 and "task status: limit_reached" in output
              and f"workflow={name}" in output and "resume=candidate" in output)
        print(f"task status exposes safe checkpoint metadata: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(original + "log PHASE_C_TAMPERED\n")
        r = run("--run", f"workflow resume {name}")
        output = r.stdout + r.stderr
        ok = (r.returncode != 0 and "changed since checkpoint" in output
              and "PHASE_C_LAST" not in output)
        print(f"edited workflow is rejected on resume: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(original)
        r = run("--run", f"task resume {name}")
        output = r.stdout + r.stderr
        try:
            with open(checkpoint, "r", encoding="utf-8") as f:
                manifest = json.load(f)
            checkpoint_ok = manifest.get("status") == "completed"
        except (OSError, ValueError):
            checkpoint_ok = False
        ok = (r.returncode == 0 and "PHASE_C_LAST" in output
              and "PHASE_C_FIRST" not in output and checkpoint_ok)
        print(f"resume continues after the last completed step: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        # A crash during an action is ambiguous; automatic replay must fail closed.
        with open(checkpoint, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        manifest["status"] = "step_in_progress"
        with open(checkpoint, "w", encoding="utf-8") as f:
            json.dump(manifest, f)
        r = run("--run", f"workflow resume {name}")
        output = r.stdout + r.stderr
        ok = r.returncode != 0 and "not safe to resume" in output
        print(f"uncertain in-progress action is not replayed automatically: {'PASS' if ok else 'FAIL'}")
        failures += not ok
    finally:
        for p in (path, checkpoint, outcomes):
            try:
                if p in backups:
                    with open(p, "wb") as f:
                        f.write(backups[p])
                elif os.path.isfile(p):
                    os.remove(p)
            except OSError:
                pass

    print(f"\n{5 - failures}/5 Phase C checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
