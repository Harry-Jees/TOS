"""Self-test: prove the sandbox boundary actually holds.

Run from the CLI (`python run.py --self-test`) or from the desktop app's
Machine menu. Every check is a claim about the sandbox that the user is
entitled to see verified rather than assumed.
"""

from __future__ import annotations

from .kernel import Kernel
from .shell import ShellSession


def run_self_test(kernel: Kernel) -> tuple[list[tuple[str, bool, str]], int]:
    """Return (checks, failed_count)."""
    checks: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append((name, bool(ok), detail))

    vfs = kernel.vfs

    for bad in ("/../../etc/passwd", "..\\..\\windows\\system32", "C:\\Windows\\System32", "/.."):
        try:
            vfs.real(bad)
            check(f"reject {bad!r}", False, "escape was NOT rejected")
        except Exception as exc:
            check(f"reject {bad!r}", True, type(exc).__name__)

    vfs.write_text("/home/user/selftest.txt", "hello sandbox\n")
    check("vfs write + read round trip",
          vfs.read_text("/home/user/selftest.txt") == "hello sandbox\n")

    result = kernel.shell_exec(ShellSession(kernel), "echo hi | wc -c")
    fields = result.out.split()
    check("shell pipeline works", result.code == 0 and fields[-1] == "3", result.text[:60])

    result = kernel.shell_exec(ShellSession(kernel), "python -c 'print(1)'")
    check("no host binaries in the shell", result.code == 127, result.stderr[:80])

    result = kernel.shell_exec(ShellSession(kernel), "cat /etc/passwd")
    check("no host /etc to read", "no such file" in result.text.lower(), result.text[:80])

    result = kernel.shell_exec(ShellSession(kernel), "rm -r /")
    check("root cannot be deleted from inside", result.code != 0 and vfs.exists("/etc/motd"),
          result.text[:60])

    png = kernel.screen.screenshot_png(force=True)
    check("screen renders to a PNG", png.startswith(b"\x89PNG") and len(png) > 2000, f"{len(png)} bytes")

    info = kernel.sysinfo()
    check("windows are open", info["windows"] >= 1, f"{info['windows']} windows")

    vfs.remove("/home/user/selftest.txt")
    failed = sum(1 for _, ok, _ in checks if not ok)
    return checks, failed


def format_report(checks: list[tuple[str, bool, str]], failed: int) -> str:
    lines = ["SandboxOS self-test", "-------------------"]
    for name, ok, detail in checks:
        lines.append(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""))
    lines.append("")
    lines.append(f"{len(checks) - failed}/{len(checks)} checks passed")
    return "\n".join(lines)
