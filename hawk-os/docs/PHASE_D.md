# Phase D — Workflow Preflight Review

## Goal

Make saved automation inspectable before it touches the desktop. This follows a common pattern in open-source computer-use agents: separate planning/review from execution, classify actions by their likely impact, and make the permission boundary visible rather than hiding it inside the model loop.

## Command

`workflow inspect <name>`

The command:

- Resolves nested saved workflow references using the same depth and size limits as execution.
- Compiles variables and conditional branches before review, so malformed workflows fail before any action runs.
- Reports the number of executable commands and a line-numbered list of command classes that deserve review.
- Avoids printing command arguments, typed values, URLs, or other payload text into the preflight report.
- Never executes desktop actions, even if the application is not in dry-run mode.

## Risk labels

The first version uses deterministic command-pattern heuristics to flag deletion, browser navigation/submission, UI Automation changes, external launches, text entry, clicks/drags, and potentially submitting or closing keyboard shortcuts.

These labels are advisory, not a security sandbox. A harmless-looking coordinate click can still hit a destructive button, and an unflagged command can still have side effects. Existing per-action approval checks remain authoritative.

## Validation

The Windows workflow regression suite verifies that preflight detects a browser navigation step and does not execute an adjacent `log` command. CI runs the test after building Hawk. Manual review on a disposable Windows session is still required for live desktop behavior.
