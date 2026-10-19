# HAWK OS Linux — Sandbox Research Agent

Linux-native companion to HAWK OS for authorized sandbox security assessment. It inventories Docker/Podman containers and checks exposed configuration; it does not attempt sandbox escape or probe private infrastructure.

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

## Local AI brain (Hugging Face)
Default model: [Qwen/Qwen3.5-4B](https://huggingface.co/Qwen/Qwen3.5-4B), Apache-2.0 licensed and multimodal. The checkpoint is about 9.3 GB before runtime overhead, so reserve extra disk and RAM/VRAM. Since target hardware is unknown, this is a practical starting point—not a claim that it is the largest model your machine can run.

```bash
python -m pip install -e '.[model]'
hawk-linux model-download
hawk-linux ask "Explain this sandbox audit finding and suggest safe remediation."
```
Choose another compatible Hugging Face model with `--model ORG/REPO`. Downloading is explicit. Model output is advisory and must never be piped into a shell; a separate policy layer must approve every future system action.

## Checks
Privileged mode, host PID/network/IPC namespaces, sensitive writable mounts, runtime sockets, extra Linux capabilities, unconfined seccomp/AppArmor, writable root filesystems, and unspecified container user.

Reports are heuristic, not proof of exploitability. Environment values are never exported; only variable names are collected. Inventory is read-only and never executes commands inside target containers.

Use only on systems you own or are explicitly authorized to assess. OpenAI-managed sandbox testing requires an authorized scope; this tool cannot access OpenAI private infrastructure merely by running on Linux.

## Roadmap
Policy/approval broker, tamper-evident logs, disposable local test harness, optional PySide6 UI, and a pluggable model adapter. The model proposes actions; a separate policy layer must approve and constrain them.
