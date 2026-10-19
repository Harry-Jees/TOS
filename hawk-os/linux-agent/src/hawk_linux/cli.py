"""Command-line interface."""
import argparse, json, platform
from datetime import datetime, timezone
from pathlib import Path
from . import __version__
from .runtime import discover, inventory
from .audit import audit

def main() -> int:
    p = argparse.ArgumentParser(prog="hawk-linux", description="HAWK OS Linux sandbox inspection")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("doctor", help="check OS and available container runtimes")
    sub.add_parser("inventory", help="list local Docker/Podman container metadata")
    a = sub.add_parser("audit", help="run read-only configuration checks")
    a.add_argument("--output", default="", help="write JSON report to this path")
    args = p.parse_args()
    if args.command == "doctor":
        print(json.dumps({"version": __version__, "platform": platform.platform(), "runtimes": discover()}, indent=2))
        return 0
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
