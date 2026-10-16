#!/usr/bin/env python3
"""SandboxOS launcher.

  python run.py                      boot the machine and serve the console
  python run.py --task "..."         boot, then give the agent that task
  python run.py --headless --task .. run a task with no server and print the result
  python run.py --self-test          prove the sandbox boundaries hold
  python run.py --scripted           force the offline scripted brain
"""

from __future__ import annotations

import argparse
import sys
import time

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))

from agentd import AgentRunner, ScriptedProvider, build_provider  # noqa: E402
from sandboxos import config  # noqa: E402
from sandboxos.kernel import Kernel  # noqa: E402
from sandboxos.selftest import format_report, run_self_test  # noqa: E402


def build_kernel(args) -> Kernel:
    return Kernel(boot_windows=not args.no_windows)


def selftest(kernel: Kernel) -> int:
    """Print the sandbox boundary self-test and return a shell exit code."""
    checks, failed = run_self_test(kernel)
    print("\n" + format_report(checks, failed))
    return 1 if failed else 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Boot SandboxOS")
    parser.add_argument("--port", type=int, default=int(config.get("SANDBOX_PORT", "8787")))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--task", help="task to hand the agent on startup")
    parser.add_argument("--headless", action="store_true", help="run the task, print the summary, exit")
    parser.add_argument("--scripted", action="store_true", help="force the offline scripted brain")
    parser.add_argument("--model", help="override the model name")
    parser.add_argument("--no-windows", action="store_true", help="boot without opening app windows")
    parser.add_argument("--self-test", action="store_true", help="verify the sandbox boundary and exit")
    parser.add_argument("--no-serve", action="store_true", help="boot without the http console")
    parser.add_argument("--desktop", action="store_true", help="open the native desktop app instead of a browser")
    parser.add_argument("--scale", type=float, default=None, help="desktop app screen scale (1.0 or 0.5)")
    args = parser.parse_args(argv)

    config.ensure_dirs()
    kernel = build_kernel(args)

    if args.self_test:
        return selftest(kernel)

    provider = ScriptedProvider() if args.scripted else build_provider(kernel, model=args.model)
    runner = AgentRunner(kernel, provider=provider)

    if args.desktop:
        import desktop

        if args.task:
            runner.submit(args.task)
        return desktop.run(kernel, runner, scale=args.scale)

    server = None
    if not args.headless and not args.no_serve:
        from server import serve  # local import keeps headless mode dependency-free

        server = serve(kernel, runner, host=args.host, port=args.port)
        print(r"  ____                  _                 _    ___  ____  ")
        print(r" / ___|  __ _ _ __   __| |_ __   ___   __| |  / _ \/ ___| ")
        print(r" \___ \ / _` | '_ \ / _` | '_ \ / _ \ / _` | | | | \___ \ ")
        print(r"  ___) | (_| | | | | (_| | |_) | (_) | (_| | | |_| |___) |")
        print(r" |____/ \__,_|_| |_|\__,_|_.__/ \___/ \__,_|  \___/|____/ ")
        print()
        print(f"  console      {server.url}")
        print(f"  screen       {kernel.screen.width}x{kernel.screen.height} virtual framebuffer")
        print(f"  sandbox disk {kernel.root}")
        print(f"  agent brain  {getattr(provider, 'description', provider.name)}")
        print(f"  screenshots  {config.SHOTS_DIR}")
        print(f"  audit log    {kernel.audit.path}")
        if not config.api_key():
            print("\n  No API key found. Using the built-in scripted brain.")
            print("  Add SANDBOX_LLM_API_KEY=... (or NVIDIA_API_KEY=...) to .env for real reasoning.")
        print("\n  Ctrl-C to stop the machine.\n")

    if args.task:
        summary = runner.run(args.task) if args.headless else (runner.submit(args.task), "started")[1]
        if args.headless:
            print(f"\nagent summary:\n{summary}\n")

    if args.headless or args.no_serve:
        return 0

    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nshutting down SandboxOS...")
    finally:
        if server:
            server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
