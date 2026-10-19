"""Phase B workflow-library regressions. Run after building Hawk on Windows."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "hawk.exe")
WORKFLOWS = os.path.join(ROOT, "workspace", "workflows")


def run(*args):
    return subprocess.run(
        [EXE, *args], cwd=ROOT, capture_output=True, text=True,
        timeout=30, errors="replace"
    )


def main():
    if not os.path.isfile(EXE):
        print("FAIL: hawk.exe not found; build Hawk first")
        return 2

    os.makedirs(WORKFLOWS, exist_ok=True)
    suffix = str(os.getpid())
    names = [
        f"phase-b-probe-{suffix}",
        f"phase-b-cycle-a-{suffix}",
        f"phase-b-cycle-b-{suffix}",
        f"phase-b-large-{suffix}",
        f"phase-b-inspect-{suffix}",
    ]
    paths = [os.path.join(WORKFLOWS, name + ".hawk") for name in names]
    previous = {}
    for path in paths:
        if os.path.isfile(path):
            with open(path, "rb") as f:
                previous[path] = f.read()
    failures = 0
    try:
        with open(paths[0], "w", encoding="utf-8", newline="") as f:
            f.write("wait 99999999\n")
        r = run("--dry-run", "--run", f"workflow run {names[0]}")
        output = r.stdout + r.stderr
        ok = (
            r.returncode == 0
            and "expanded saved workflow reference(s)" in output
            and "waiting 30000 ms" in output
        )
        print(f"saved workflow expands before execution: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        with open(paths[1], "w", encoding="utf-8", newline="") as f:
            f.write(f"workflow run {names[2]}\n")
        with open(paths[2], "w", encoding="utf-8", newline="") as f:
            f.write(f"workflow run {names[1]}\n")
        r = run("--dry-run", "--run", f"workflow run {names[1]}")
        output = r.stdout + r.stderr
        ok = r.returncode != 0 and "nesting exceeds the 4-level limit" in output
        print(f"cyclic workflow nesting fails closed: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        r = run("--dry-run", "--run", f"workflow run missing-phase-b-{suffix}")
        output = r.stdout + r.stderr
        ok = r.returncode != 0 and "workflow expansion failed" in output
        print(f"missing workflow fails before execution: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        r = run("--dry-run", "--run",
                f"log PHASE_B_MUST_NOT_RUN\nworkflow run {names[0]} unexpected")
        output = r.stdout + r.stderr
        ok = (r.returncode != 0
              and "invalid workflow run syntax" in output
              and "PHASE_B_MUST_NOT_RUN" not in output)
        print(f"malformed workflow fails before any step: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        r = run("--dry-run", "--run", "workflow run ..")
        output = r.stdout + r.stderr
        ok = r.returncode != 0 and "workflow expansion failed" in output
        print(f"path-traversal workflow name is rejected: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        with open(paths[3], "wb") as f:
            f.write(b"#" * (1024 * 1024 + 1))
        r = run("--dry-run", "--run", f"workflow run {names[3]}")
        output = r.stdout + r.stderr
        ok = r.returncode != 0 and "1 MiB size limit" in output
        print(f"oversized workflow is rejected: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        logic = 'set app Notepad\nif app == Notepad\nlog PHASE_B_BRANCH_TRUE\nelse\nlog PHASE_B_BRANCH_FALSE\nendif\nlog APP_$app'
        r = run("--dry-run", "--run", logic)
        output = r.stdout + r.stderr
        ok = (r.returncode == 0 and "PHASE_B_BRANCH_TRUE" in output
              and "PHASE_B_BRANCH_FALSE" not in output and "APP_Notepad" in output)
        print(f"workflow variables and branches: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        r = run("--dry-run", "--run", "log PHASE_B_MALFORMED\nif x == y\nlog MUST_NOT_RUN")
        output = r.stdout + r.stderr
        ok = (r.returncode != 0 and "unclosed if block" in output
              and "PHASE_B_MALFORMED" not in output and "MUST_NOT_RUN" not in output)
        print(f"malformed branch fails before execution: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        r = run("--dry-run", "--run", 'browser text "body"')
        output = r.stdout + r.stderr
        ok = r.returncode == 0 and "dry-run: browser text body" in output
        print(f"browser DOM command supports dry-run: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        r = run("--dry-run", "--run", "workflow stats")
        output = r.stdout + r.stderr
        ok = r.returncode == 0 and "workflow memory:" in output
        print(f"workflow outcome memory is queryable: {'PASS' if ok else 'FAIL'}")
        failures += not ok

        inspect_side_effect = os.path.join(ROOT, "workspace", "phase-d-inspect-must-not-run.txt")
        if os.path.exists(inspect_side_effect):
            os.remove(inspect_side_effect)
        with open(paths[4], "w", encoding="utf-8", newline="") as f:
            f.write("write phase-d-inspect-must-not-run.txt sentinel\\nbrowser navigate https://example.com\\n")
        # Intentionally omit --dry-run: inspect itself must never execute workflow steps.
        r = run("--run", f"workflow inspect {names[4]}")
        output = r.stdout + r.stderr
        ok = (r.returncode == 0 and "workflow preflight:" in output
              and "review item(s)" in output and "browser" in output
              and "sentinel" not in output and not os.path.exists(inspect_side_effect))
        print(f"workflow preflight is read-only outside dry-run: {'PASS' if ok else 'FAIL'}")
        failures += not ok
        if os.path.exists(inspect_side_effect):
            os.remove(inspect_side_effect)

    finally:
        for path in paths:
            try:
                if path in previous:
                    with open(path, "wb") as f:
                        f.write(previous[path])
                elif os.path.isfile(path):
                    os.remove(path)
            except OSError:
                pass

    print(f"\n{11 - failures}/11 workflow checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
