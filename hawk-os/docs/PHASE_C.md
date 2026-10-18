# Phase C — Resilient Long-Running Tasks

Phase C adds safe recovery for reusable workflow runs without treating a checkpoint as permission to repeat uncertain actions.

## Capabilities

- **Durable fingerprints:** atomic checkpoints include workflow name, a stable script fingerprint, process ID, attempted step count, and the next safe script offset. They do not store command text, prompts, screenshots, or credentials.
- **Resume from known boundaries:** use `workflow resume <name>` after a workflow stops at a command limit or is aborted before the next command. Resume requires the same saved workflow contents and a line-boundary offset.
- **Fail-closed crash recovery:** a checkpoint marked `step_in_progress` or `failed` is not automatically replayed because the last action may have partially completed. Inspect the desktop and restart or repair the workflow manually.
- **Stale-task protection:** a running checkpoint is rejected while its recorded process is still alive. Script fingerprints prevent resuming a workflow that was edited after the checkpoint.
- **Outcome memory:** Phase B's local run journal and `workflow stats` / `workflow recommend` remain available.

## Use

1. Run a saved workflow normally: `workflow run <name>`.
2. If it reaches the configured step limit or is stopped before a command begins, run `workflow resume <name>`.
3. Review the screen and workflow after any failure. A partially executed action is never replayed automatically.

The resume command continues at the next safe boundary, not at an arbitrary instruction pointer. It is deliberately unavailable in dry-run mode and cannot resume a plain ad-hoc script.

## Validation

Run `python tools/verify_phase_c.py` after building Hawk on Windows. The suite verifies checkpoint metadata, continuation after a step limit, rejection of modified workflows, and refusal to replay an action whose execution state is uncertain.
