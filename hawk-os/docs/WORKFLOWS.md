# Phase B — Reusable Workflow Library

## User workflow

1. Run a goal in Hawk so the planner generates a script.
2. In the script runner, execute `workflow save <name>` and approve the save prompt.
3. Use `workflow list` to inspect available saved workflows.
4. Add `workflow run <name>` as a standalone line in a script to expand and execute that recipe.

Workflows are stored under `workspace/workflows/<name>.hawk`. Names allow 1–48 letters, digits, hyphens, and underscores. Existing workflows are never overwritten by `workflow save`; choose a new name if you want to preserve both versions.

## Safety and limits

- Saving requires approval because a generated script may contain literal text inputs.
- Workflow expansion occurs before execution; the expanded actions still pass through Hawk's ordinary action checks and approval prompts.
- Each workflow file and the expanded script are limited to 1 MiB; nested workflow expansion is limited to four levels.
- Reparse-point paths and unsafe workflow names are rejected.
- Workflow save directives are stripped from the saved recipe to avoid a workflow trying to save itself again.
- Workflows are ordinary local scripts, not a sandbox. Review them before saving or running. A recipe can still operate on the signed-in desktop within Hawk's existing permissions.

## Variables and branching

Workflows also support bounded compile-time variables and equality branches:

```text
set target "Notepad"
if target == Notepad
  open notepad
else
  log target did not match
endif
```

Use `$name` to substitute a variable into a command. Values are limited to 4096 bytes, at most 64 variables and 16 nested conditional blocks are allowed. Conditions are literal equality checks, not a general-purpose expression evaluator. The compiler validates the whole structure before any desktop action runs.

## Outcome records

Non-dry-run script executions append only aggregate status, step count, and failure count to `workspace/run-outcomes.jsonl`. The journal intentionally excludes prompts, typed text, screenshots, window titles, and file contents. It provides a local signal for later opt-in learning; it does not autonomously change the planner or train a model.

## Remaining limitations

Raw mouse/keyboard demonstration recording and a live browser DOM/CDP automation engine are not implemented in this change. Browser automation requires a dedicated, opt-in debugging connection and permission model; do not enable Chrome remote debugging on untrusted networks. These should not be represented as complete until implemented and tested end-to-end.
