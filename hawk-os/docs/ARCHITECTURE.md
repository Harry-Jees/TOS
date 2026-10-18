# Hawk architecture

## Product direction

Hawk is a Windows desktop AI agent, not an operating system. The goal is a dependable local desktop executor with a polished, inspectable experience: describe a task, review a plan, approve actions, watch progress, and inspect the result. It should feel closer to a desktop coworker than a command launcher.

Hawk must be designed around one shared task-execution core so future Android and WhatsApp clients become controlled request channels—not separate automation engines.

## Current implementation

- **Win32 UI**: live screen preview, goal/script inputs, run controls, and activity log.
- **Agent loop**: screenshot → model plan → command execution → fresh screenshot → verification/correction.
- **Command runtime**: a small Hawk command language for screenshots, cursor/keyboard/mouse, windows, and workspace files.
- **Perception**: target-window-aware Windows UI Automation tree traversal plus local pixel/region detection and model vision. UIA may be incomplete for custom-drawn or protected applications.
- **Semantic actions**: invoke, set value, toggle, select, expand/collapse, and focus through native UIA control patterns. Exact accessible names are preferred; ambiguous matches fail closed and the agent requests fresh visual verification after semantic actions.
- **Model transport**: WinHTTP requests to an OpenAI-compatible chat-completions endpoint.
- **Persistence**: local settings, logs, screenshots, and reusable scripts.

## Target architecture

Keep model reasoning separate from trusted execution:

```text
Win32 UI / Android / WhatsApp adapter
                  |
          Authenticated task gateway
                  |
       TaskController + state machine
                  |
         Policy + approval broker
                  |
        Typed action/tool interface
          /          |          \
     UI Automation  Screen     Workspace
         (UIA)      capture    file tools
          \          |          /
          Observation + verifier
                  |
         Model/provider adapter
```

The model proposes typed actions; it must not directly decide its own permissions. The policy layer validates every action, its target, scope, risk, timeout, and required approval before the runtime executes it. Every remote client must call the same task controller and policy engine as the local UI.

## Non-negotiable design rules

1. **Fail closed.** Model execution is opt-in. Missing/invalid policy, credentials, target, or confirmation means no action.
2. **Bound file access.** File tools operate in an explicit workspace. Resolve canonical paths and reject traversal and reparse-point escapes. Existing files require a deliberate overwrite path.
3. **Least privilege.** Do not run elevated by default. Never ask users to disable Windows security controls.
4. **Observable actions.** Each action has an ID, start/end time, typed result, error details, and optional postcondition. A model claim is not proof of success.
5. **Human approval for high impact.** File deletion/overwrite, external sends, purchases, credential entry, security settings, and other irreversible actions require approval. A message from a remote channel cannot count as approval for high-risk actions.
6. **Privacy by design.** Tell users what is captured and which provider receives it. The desktop UI asks for consent before a vision goal; CLI users must pass `--allow-screen-upload`. The model client rejects image-bearing requests without that per-run approval. Redact sensitive regions where feasible.
7. **Provider abstraction.** Model selection, retries, structured outputs, and provider-specific request details live behind one adapter. Secrets never appear in logs or source.
8. **Recoverable workflows.** Persist checkpoints and action outcomes, not just an opaque transcript. Resume only after revalidating the current desktop state.
9. **Remote access is paired and revocable.** No public inbound desktop port. Prefer an authenticated outbound PC connection; validate expiry, nonces, replay protection, rate limits, and identity binding.
10. **Tests are layered.** Unit-test parsing/policy/state transitions offline; mock model/network responses; keep real desktop tests explicitly opt-in and isolated.

## Phase 1 scope

Implemented in this branch: safer execution defaults, workspace-scoped file commands, DPAPI-backed saved credentials, HTTPS enforcement for non-loopback model endpoints, per-action approval for selected high-impact commands, input-result checks, structural JSON decoding, initial UI Automation observations, offline regression scripts, Windows CI configuration, and security/open-source documentation.

Still follow-on work (not implied to be complete by this branch): a fully typed action API replacing the command DSL, comprehensive policy coverage for every possible side effect, cancellable network requests, durable checkpoints/resume, crash recovery, adversarial prompt-injection testing, robust postcondition verification for all action types, signed releases, and paired remote clients. The current protections are defense-in-depth, not an OS sandbox; do not use Hawk as an unattended or unrestricted system administrator.

## Suggested module boundaries

- `TaskController`: lifecycle, cancellation, checkpoints, retries, and status events.
- `PolicyEngine`: action allowlists, workspace boundaries, risk classifications, and approvals.
- `ActionRuntime`: validated typed actions and real Win32 execution.
- `Perception`: Windows UI Automation first, screenshot/vision fallback.
- `ModelClient`: request/response schema validation, timeouts, cancellation, and provider configuration.
- `Verifier`: independent postconditions and evidence-based completion.
- `AuditLog`: redacted structured events and local retention controls.
- `PairingService`: device/client pairing, identity binding, revocation, and session expiry.
- `RemoteGateway`: authenticated task/status relay; it never executes desktop actions itself.
- `ChannelAdapter`: maps Android or WhatsApp messages to a narrow, validated task/status protocol.

## Roadmap

### Stabilize
- Finish Windows build and regression validation for the current hardening changes.
- Replace the legacy response extraction with a maintained JSON parser when dependency policy is decided.
- Keep semantic UIA actions as the first choice for accessible controls, with screenshot-grounded input as fallback and fresh observation after state-changing actions.
- Add cancellable WinHTTP requests and bounded response sizes.
- Add action policy/approval UI and typed postconditions.
- Add regression tests for path escapes, overwrite attempts, malformed JSON, Unicode, and interrupted actions.

### Improve desktop capability
- Harden UI Automation with app-specific tests, stronger target-window detection, and safe handling of inaccessible/custom-drawn apps; retain vision/coordinate fallback for unsupported controls.
- Add additional semantic patterns (scroll, text ranges, grids/tables) only with bounded targets and postcondition verification.
- Replace direct model-generated command strings with a typed action schema validated by the policy engine.
- Add per-step plan review, task timeline, durable checkpoints, stop/cancel, and evidence-based result verification.
- Add browser and application skills only through explicit, permissioned tools.

### Remote clients
- First implement the local TaskController and mock client without a network listener.
- Then implement secure pairing and an outbound authenticated relay, followed by Android.
- Add WhatsApp only through the official WhatsApp Business Platform API and verified webhooks; bind sender identities to paired installations, rate-limit, deduplicate webhook retries, and keep approvals local.
- Do not expose arbitrary shell, raw mouse/keyboard, API keys, screenshots, or high-risk approval through remote channels.

See [MOBILE_COMPANION.md](MOBILE_COMPANION.md) and [REMOTE_CONTROL_ARCHITECTURE.md](REMOTE_CONTROL_ARCHITECTURE.md) for the remote protocol, rollout gates, security requirements, and vendor references.

## Reference patterns from established platforms

These are design references, not a claim that Hawk matches their capabilities:

- [OpenAI computer-use documentation](https://developers.openai.com/api/docs/guides/tools-computer-use) describes a driver-mediated action loop, fresh observations, persistent environment state, and explicit result verification.
- [Anthropic computer-use documentation](https://platform.claude.com/docs/en/agents-and-tools/tool-use/computer-use-tool) highlights screenshots, structured desktop actions, user consent, and prompt-injection risks.
- [Microsoft UI Automation](https://learn.microsoft.com/en-us/windows/win32/winauto/entry-uiauto-win32) provides semantic access to Windows controls and should be preferred over coordinate-only interaction when supported.
- [WhatsApp Cloud API](https://developers.facebook.com/docs/whatsapp/cloud-api/) and [WhatsApp webhooks](https://developers.facebook.com/docs/whatsapp/cloud-api/webhooks) are the official basis for a future WhatsApp channel.

## Current limitations

This is an evolving prototype. The desktop command runtime has broad control over the signed-in Windows session and is not a hardened sandbox. The architecture described above is a target, not a claim that every component exists today. Remote control is not implemented merely because its design is documented.
