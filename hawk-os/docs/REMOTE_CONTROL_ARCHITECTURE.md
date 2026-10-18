# Hawk remote companion plan

Hawk's future remote clients should submit authenticated **task requests**, not gain a raw remote desktop shell. The Windows agent remains the sole authority that observes the desktop, checks policy, requests approval, and executes actions.

## Client experiences

### Android application
- Push-to-talk or tap-to-record; show the transcript for editing before submission.
- Show task plan, PC online/offline status, step progress, approval requests, and result.
- Send follow-ups and pause/stop tasks; receive completion/error notifications.
- Pair by scanning a short-lived QR code displayed and confirmed on the PC.

### WhatsApp agent (future channel)
- Let an explicitly paired user send a text task and receive status updates/results in WhatsApp.
- Use the official WhatsApp Business Platform Cloud API through a server-side adapter and verified webhooks; do not automate WhatsApp Web or scrape personal chats.
- Bind the WhatsApp sender identity to a specific paired Hawk installation. Unknown numbers must never enqueue work.
- Return a short task ID and state; keep screenshots, file contents, credentials, and sensitive output out of messages by default.
- Require a second confirmation in the trusted desktop UI for high-impact actions. A WhatsApp message alone must not enable Auto-run, change policy, or approve sensitive actions.
- Respect WhatsApp's messaging window/template rules and the user's opt-in/opt-out state.

## Security architecture

```text
Android app ───────┐
                   ├─> Authenticated task gateway / relay
WhatsApp Cloud API ┘          |
                         Identity + pairing
                         Rate limits + replay checks
                         Task schema validation
                              |
                    Outbound authenticated channel
                              |
                     Hawk Windows service/app
                              |
                    Local policy + approval gate
                              |
                    Typed desktop action runtime
                              |
                     Observe / verify / report
```

The gateway is a request broker, not a desktop executor. Prefer an outbound PC connection so the Windows machine does not need a public inbound port. The relay should carry only minimum task/status metadata; do not route provider API keys through it.

## Security requirements

1. Pairing starts from the Windows PC and is confirmed locally. QR challenges are short-lived, one-time, and contain no permanent secret.
2. Each device/client has a revocable identity. Authenticate messages with TLS, expiry, nonces, and replay protection.
3. For WhatsApp webhooks, validate the provider's authenticity/signature using the official verification procedure; enforce sender allowlists, rate limits, message-size limits, and idempotency.
4. The desktop owns model-provider credentials. Never send them to mobile, WhatsApp, or the relay.
5. All channels call the same task API and policy engine. They cannot send arbitrary shell commands, raw mouse coordinates, or policy overrides.
6. High-risk actions (deletion/overwrite, external messages, purchases, credential entry, security settings) require local approval and an auditable decision.
7. Keep a device/client list, last-seen time, revoke control, expiry, audit history, and emergency stop.
8. Voice is push-to-talk by default. Show recording state and provide transcript deletion controls.
9. Screen preview is off by default. Share only after per-session consent, with redaction where feasible.
10. Store mobile keys in Android Keystore. Keep WhatsApp access tokens and app secrets server-side in a secret manager. Never log tokens, transcripts, or private payloads.

## Proposed protocol

Use a versioned, strictly validated envelope with bounded fields:

- `version`, `message_id`, `client_id`, `installation_id`
- `task_id`, `issued_at`, `expires_at`, `nonce`
- `kind`: submit, status, cancel, approval_request, result
- `payload`: a bounded natural-language goal or typed status/result—not arbitrary executable code
- `correlation_id` and structured error codes

Reject expired messages, duplicates, unknown fields where security-sensitive, oversized payloads, and unsupported protocol versions. Use idempotency keys so webhook retries cannot create duplicate tasks.

## Rollout

1. **Local core first:** expose an internal task controller, typed actions, policy decisions, task IDs, cancellation, and structured status events. No network listener yet.
2. **Test client:** add a local/mock companion to exercise submit/status/cancel without real remote control.
3. **Secure pairing and relay:** short-lived pairing, revocation, outbound authenticated PC connection, expiry/replay protection, audit logs.
4. **Android:** transcript review, task submission, progress, approvals, stop, and notifications.
5. **WhatsApp:** official Cloud API webhook adapter; sender-to-installation binding; text-only tasks and status; local approvals remain mandatory.
6. **Remote screen preview:** only after consent, redaction, authorization, and retention controls.
7. **Release gate:** threat-model and test token theft, replay, duplicate webhooks, unauthorized senders, relay compromise, prompt injection, lost phones, and PC disconnection before public remote access.

## Not in the first remote release

- No public inbound desktop port.
- No unrestricted remote keyboard/mouse or shell endpoint.
- No remote Auto-run enablement or remote approval for high-risk actions.
- No always-on microphone or default screenshot forwarding.
- No API keys on clients or relay.
- No claim that the current prototype is safe for unattended operation.

## Official reference material

- [OpenAI computer-use guide](https://developers.openai.com/api/docs/guides/tools-computer-use): action-driver contract, persistent environment, screenshots, and explicit verification.
- [Anthropic computer-use tool](https://platform.claude.com/docs/en/agents-and-tools/tool-use/computer-use-tool): screenshot/action loop, user consent, prompt-injection risks, and step-by-step verification.
- [Microsoft UI Automation](https://learn.microsoft.com/en-us/windows/win32/winauto/entry-uiauto-win32): semantic access to Windows controls, preferred before coordinate-only interaction where supported.
- [WhatsApp Business Platform Cloud API](https://developers.facebook.com/docs/whatsapp/cloud-api/): official server-side WhatsApp integration.
- [WhatsApp webhooks](https://developers.facebook.com/docs/whatsapp/cloud-api/webhooks): inbound event delivery and webhook integration.

These are architecture references, not endorsements or claims that Hawk already implements their capabilities.
