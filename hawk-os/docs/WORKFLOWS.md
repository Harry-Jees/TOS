# Phase B — Hybrid Execution and Reusable Task Memory

## Reusable workflows

- Save a generated script with `workflow save <name>` (approval required).
- Inspect saved recipes with `workflow list`.
- Add `workflow run <name>` as a standalone line to expand a recipe before execution.
- Names allow 1–48 letters, digits, hyphens, and underscores. Existing workflows are never overwritten.
- Files live in `workspace/workflows/<name>.hawk`; each file and expanded script is limited to 1 MiB, with four levels of nesting maximum.

## Variables and branches

The workflow compiler supports bounded compile-time variables and equality conditions:

```text
set app Notepad
if app == Notepad
  open notepad
else
  log app did not match
endif
```

Use `$app` to substitute a value into a command. Values are limited to 4096 bytes, 64 variables per script, and 16 nested conditionals. Only literal equality is supported; this is not a general expression language. The full control structure is validated before physical actions begin.

## Recording mouse and keyboard demos

1. Run `workflow record start` and approve the global-input recording prompt.
2. Perform the demonstration. Mouse clicks, left-button drags, and supported key chords are captured locally.
3. Run `workflow record stop <name>` and approve saving the recipe.
4. Review the saved script before replaying it.

Recording is explicit, global, and can capture sensitive keystrokes. Do not enter passwords, payment details, API keys, or other secrets while it is active. The recorder stores supported key chords, not a hidden keystroke log; unsupported keys are skipped. Recorded coordinates are specific to the current display layout.

## Browser DOM adapter

Hawk can use a local Chrome DevTools Protocol (CDP) endpoint when the user deliberately starts a browser with remote debugging enabled on localhost port 9222. Keep that endpoint bound to loopback and never expose it to a network. Commands:

- `browser status` — verify a debuggable page is available.
- `browser text "body"` — read up to 4000 characters from a CSS selector.
- `browser click "button[type=submit]"` — click a matching element (approval required).
- `browser fill "input[name=email]" "value"` — fill a field (approval required; value is redacted from the approval prompt).
- `browser navigate https://example.com` — navigate the active page (approval required).

The adapter targets the first available page and intentionally exposes a small set of DOM operations rather than arbitrary JavaScript execution. Browser-side content is untrusted. CDP is a privileged interface; only enable it for a local session you control. Dry-run mode never connects to the browser.

## Local outcome memory

Non-dry-run executions append only status, workflow name (when launched through `workflow run <name>`), step count, and failure count to `workspace/run-outcomes.jsonl`. It does not store prompts, screenshots, window titles, or command text. Use `workflow stats` to review outcomes and `workflow recommend` to rank saved workflows by observed success with a small-sample smoothing factor. This is local evidence-based ranking, not model training or automatic script rewriting.

## Safety and limits

- Expanded actions still pass through Hawk's normal action checks and approval prompts.
- Unsafe names, oversized workflows, malformed references, and reparse-point paths are rejected.
- Workflows are local scripts, not a sandbox. Review before running; they can operate on the signed-in desktop within Hawk's existing permissions.
- CDP requires a compatible Chromium browser with remote debugging enabled; browser-specific and authenticated sites may behave differently.
