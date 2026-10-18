# Hawk architecture

## Product direction

Hawk is a Windows desktop AI agent, not an operating system. The goal is a dependable local desktop executor with a polished, inspectable experience: describe a task, review a plan, approve actions, watch progress, and inspect the result. It should feel closer to a desktop coworker than a command launcher.

## Current implementation

- **Win32 UI**: live screen preview, goal/script inputs, run controls, and activity log.
- **Agent loop**: screenshot → model plan → command execution → fresh screenshot → verification/correction.
- **Command runtime**: a small Hawk command language for screenshots, cursor/keyboard/mouse, windows, and workspace files.
- **Perception**: initial Windows UI Automation (UIA) tree traversal plus local pixel/region detection and model vision. UIA may be incomplete for custom-drawn or protected applications.
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

## Phase 1 scope

Implemented in this branch: safer execution defaults, workspace-scoped file commands, DPAPI-backed saved credentials, HTTPS enforcement for non-loopback model endpoints, per-action approval for selected high-impact commands, input-result checks, structural JSON decoding, initial UI Automation observations, offline regression scripts, Windows CI configuration, and security/open-source documentation.

Still follow-on work (not implied to be complete by this branch): a fully typed action API replacing the command DSL, comprehensive policy coverage for every possible side effect, cancellable network requests, durable checkpoints/resume, crash recovery, adversarial prompt-injection testing, robust postcondition verification for all action types, signed releases, and a paired Android application. The current protections are defense-in-depth, not an OS sandbox; do not use Hawk as an unattended or unrestricted system administrator.

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
- Harden the initial UI Automation integration with app-specific tests, control targeting, and safe handling of inaccessible/custom-drawn apps; keep vision as fallback.
- Split the monolithic agent file into the modules above.
- Add task timeline, plan review/editing, per-step approval, checkpoints, and run history.
- Add browser and application skills only through explicit, permissioned tools.

### Mobile companion
See [MOBILE_COMPANION.md](MOBILE_COMPANION.md). The Windows machine remains the execution authority; the phone submits authenticated task requests and voice transcripts, not raw mouse/keyboard access.

## Current limitations

This is an evolving prototype. The desktop command runtime has broad control over the signed-in Windows session and is not a hardened sandbox. The architecture described above is a target, not a claim that every component exists today.
