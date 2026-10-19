# Hawk Linux Agent (native implementation)

This directory is the native Linux track. It is separate from the existing Win32/C++ desktop app; the Windows executable packaged in the earlier Debian artifact is **not** a native Linux build.

## Current state

- Native Linux track initialized on branch `feature/linux-native-agent`.
- The first milestone is a read-only host/sandbox auditor with JSON output.
- The model downloader/runtime and interactive desktop controller are not yet implemented or verified.
- No model weights are claimed as downloaded. The earlier Qwen3.5-4B candidate was estimated at about 9.3 GB; confirm actual hardware, disk, model licensing and quantized artifact before selecting it.
- This tool can inspect only the machine and paths to which the process is granted access. It cannot inspect OpenAI's internal infrastructure or hidden sandbox services.

## Quick start

Requires Python 3.10+; the audit command uses only the standard library:

```bash
python3 linux/audit.py --json
python3 linux/audit.py --path .
```

The auditor is read-only. It reports OS/Python/architecture, disk and memory where available, Git status, Docker/Podman availability, and visible processes. Process command lines may contain sensitive arguments, so JSON output is local and should be reviewed before sharing.

## Build roadmap

1. **Native foundation** — read-only host/sandbox audit, JSON report, explicit scope and permission boundaries.
2. **Model runtime** — model manifest with source/license/hash, resumable download, disk-space preflight, checksum verification, quantized-model support, optional llama.cpp/Ollama backend. Never silently download multi-GB weights.
3. **Linux desktop adapter** — Wayland/X11 capability detection; screenshot and input backend selection; visible consent before screen capture or remote model upload; no elevation by default.
4. **Agent loop** — typed actions, policy gate, plan preview, per-action approval, stop/pause, timeouts, evidence-based verification and recoverable checkpoints.
5. **Sandbox audit integrations** — opt-in Docker/Podman inspection and repository/workspace audit; no container escape, privilege escalation, secret extraction, or attempts to access unrelated users' data.
6. **UI and packaging** — polished desktop control center, system tray, activity timeline, Debian package built from native Linux sources, desktop entry, uninstall/upgrade path and reproducible CI.
7. **Release gates** — static analysis, unit/regression tests, isolated disposable VM checks, manual screenshot review, package install/uninstall verification and model-license review.

## Security defaults

- Audit is read-only by default.
- Never execute model-generated shell commands directly.
- Use an allowlisted typed-action interface and validate every path after canonicalization.
- Treat repository text, web pages, screenshots and model output as untrusted input.
- Redact secrets from logs; never store API keys in source control.
- Ask before screen capture/upload, file writes/deletes, sending messages, installing packages or changing system settings.
- Do not run as root. No silent telemetry or background model download.

## Model choice

Do not hard-code a model based only on a benchmark or name. Select based on available RAM/VRAM, quantization, tool-use reliability, context length, license, inference speed and verified artifact provenance. Keep the model provider swappable; local inference is optional and should not block the app's basic audit features.
