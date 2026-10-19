# HAWK OS Linux — Sandbox Research Agent

Linux-native companion to HAWK OS for authorized sandbox security assessment. This initial version inventories Docker/Podman containers and checks exposed configuration; it does not attempt sandbox escape or probe private infrastructure.

## Quick start
Requires Python 3.11+. Optional Docker or Podman CLI.
```bash
cd hawk-os/linux-agent
python -m venv .venv && . .venv/bin/activate
python -m pip install -e .
hawk-linux doctor
hawk-linux inventory
hawk-linux audit --output report.json
```
Without installation, run with `PYTHONPATH=src python -m hawk_linux audit`.

## Checks
Privileged mode, host PID/network/IPC namespaces, sensitive writable mounts, runtime sockets, extra Linux capabilities, unconfined seccomp/AppArmor, writable root filesystems, and unspecified container user.

Reports are heuristic, not proof of exploitability. Environment values are never exported; only variable names are collected. Inventory is read-only and never executes commands inside target containers.

Use only on systems you own or are explicitly authorized to assess. OpenAI-managed sandbox testing requires an authorized scope; this tool cannot access OpenAI private infrastructure merely by running on Linux.

## Roadmap
Policy/approval broker, tamper-evident logs, disposable local test harness, optional PySide6 UI, and a pluggable model adapter. The model proposes actions; a separate policy layer must approve and constrain them.