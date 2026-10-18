# Hawk architecture

## Product direction

Hawk is a Windows desktop AI agent, not an operating system. The goal is a dependable local desktop executor with a polished, inspectable experience: describe a task, review a plan, approve actions, watch progress, and inspect the result. It should feel closer to a desktop coworker than a command launcher.

## Current implementation

- **Win32 UI**: live screen preview, goal/script inputs, run controls, and activity log.
- **Agent loop**: screenshot → model plan → command execution → fresh screenshot → verification/correction.
- **Command runtime**: a small Hawk command language for screenshots, cursor/keyboard/mouse, windows, and workspace files.
- **Perception**: local pixel/region detection plus model vision.
- **Model transport**: WinHTTP requests to an OpenAI-compatible chat-completions endpoint.
- **Persistence**: local settings, logs, screenshots, and reusable scripts.

## Target architecture

Keep model reasoning separate from trusted execution:

```text
Desktop UI / Android companion
          |
     Task API + state machine
          |
    Policy and approval gate
          |
   Typed action/tool interface
     /          |          \
 UI Automation  Screen      Workspace
   (UIA)        capture     file tools
     \          |          /
       Observation + verifier
               |
       Model/provider adapter
```

The model proposes typed actions; it must not directly decide its own permissions. The policy layer validates every action, its target, scope, risk, timeout, and required approval before the runtime executes it.

## Non-negotiable design rules

1. **Fail closed.** Model execution is opt-in. Missing/invalid policy, credentials, target, or confirmation means no action.
2. **Bound file access.** File tools operate in an explicit workspace. Resolve canonical paths and reject traversal and reparse-point escapes. Existing files require a deliberate overwrite path.
3. **Least privilege.** Do not run elevated by default. Never ask users to disable Windows security controls.
4. **Observable actions.** Each action has an ID, start/end time, typed result, error details, and optional postcondition. A model claim is not proof of success.
5. **Human approval for high impact.** File deletion/overwrite, external sends, purchases, credential entry, security settings, and other irreversible actions require approval. Stop must interrupt between actions and cancel in-flight network work.
6. **Privacy by design.** Tell users what is captured and which provider receives it. Keep screenshots local unless the user consents to a vision request. Redact sensitive regions where feasible.
7. **Provider abstraction.** Model selection, retries, structured outputs, and provider-specific request details live behind one adapter. Secrets never appear in logs or source.
8. **Recoverable workflows.** Persist checkpoints and action outcomes, not just an opaque transcript. Resume only after revalidating the current desktop state.
9. **Tests are layered.** Unit-test parsing/policy/state transitions offline; mock model/network responses; keep real desktop tests explicitly opt-in and isolated.

## Suggested module boundaries

- `TaskController`: lifecycle, cancellation, checkpoints, retries.
- `PolicyEngine`: action allowlists, workspace boundaries, risk classifications, approvals.
- `ActionRuntime`: validated typed actions and real Win32 execution.
- `Perception`: Windows UI Automation first, screenshot/vision fallback.
- `ModelClient`: request/response schema validation, timeouts, cancellation, provider configuration.
- `Verifier`: independent postconditions and evidence-based completion.
- `AuditLog`: redacted structured events and local retention controls.
- `PairingService`: future authenticated connection to the mobile companion; no network listener until security review.

## Roadmap

### Stabilize
- Finish Windows build and regression validation for the current hardening changes.
- Replace the legacy response extraction with a maintained JSON parser when dependency policy is decided.
- Add cancellable WinHTTP requests and bounded response sizes.
- Add action policy/approval UI and typed postconditions.
- Add regression tests for path escapes, overwrite attempts, malformed JSON, Unicode, and interrupted actions.

### Improve capability
- Add UI Automation (UIA) for named controls, labels, roles, and accessible state; keep vision as fallback.
- Split the monolithic agent file into the modules above.
- Add task timeline, plan review/editing, per-step approval, checkpoints, and run history.
- Add browser and application skills only through explicit, permissioned tools.

### Mobile companion
See [MOBILE_COMPANION.md](MOBILE_COMPANION.md). The Windows machine remains the execution authority; the phone submits authenticated task requests and voice transcripts, not raw mouse/keyboard access.

## Current limitations

This is an evolving prototype. The desktop command runtime has broad control over the signed-in Windows session and is not a hardened sandbox. The architecture described above is a target, not a claim that every component exists today.
