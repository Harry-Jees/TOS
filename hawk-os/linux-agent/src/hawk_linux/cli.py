"""Command-line interface."""
import argparse, json, platform
from datetime import datetime, timezone
from pathlib import Path
from . import __version__
from .runtime import discover, inventory
from .audit import audit
from .brain import DEFAULT_MODEL, download_model

def main() -> int:
    p = argparse.ArgumentParser(prog="hawk-linux", description="HAWK OS Linux sandbox inspection")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("doctor", help="check OS and available container runtimes")
    sub.add_parser("inventory", help="list local Docker/Podman container metadata")
    a = sub.add_parser("audit", help="run read-only configuration checks")
    a.add_argument("--output", default="", help="write JSON report to this path")
    d = sub.add_parser("model-download", help="download a local brain from Hugging Face")
    d.add_argument("--model", default=DEFAULT_MODEL, help=f"Hub model ID (default: {DEFAULT_MODEL})")
    d.add_argument("--local-dir", default="", help="optional destination directory")
    q = sub.add_parser("ask", help="ask the local model; requires model extras and enough RAM/VRAM")
    q.add_argument("prompt", help="question for the local model")
    q.add_argument("--model", default=DEFAULT_MODEL)
    args = p.parse_args()
    if args.command == "doctor":
        print(json.dumps({"version": __version__, "platform": platform.platform(), "runtimes": discover()}, indent=2))
        return 0
    if args.command == "model-download":
        try:
            print(download_model(args.model, args.local_dir or None))
            return 0
        except RuntimeError as e:
            p.error(str(e))
    if args.command == "ask":
        try:
            from .brain import LocalBrain
            print(LocalBrain(args.model).ask(args.prompt))
            return 0
        except (RuntimeError, OSError, ValueError) as e:
            p.error(str(e))
    names = [x["runtime"] for x in discover() if x["available"]]
    data = [inventory(n) for n in names]
    if args.command == "inventory":
        print(json.dumps(data, indent=2)); return 0
    report = {"product": "HAWK OS Linux", "version": __version__,
              "generated_at": datetime.now(timezone.utc).isoformat(),
              "scope": "local Docker/Podman metadata only; no active exploit tests",
              "runtimes": data, "findings": [f for d in data for f in audit(d)]}
    output = json.dumps(report, indent=2)
    if args.output: Path(args.output).write_text(output + "\n", encoding="utf-8")
    print(output)
    return 0
