# Hawk Android companion plan

The Android app is a remote task companion for the Windows Hawk agent. It submits tasks and displays progress; it does not directly control mouse/keyboard or bypass the PC's local policy.

## Experience
- Push-to-talk or tap-to-record; show transcript for edit/cancel before submission.
- Show PC online/offline state, reviewed plan, approval requests, progress, and final result.
- Send follow-up instructions, pause/stop tasks, and receive completion/error notifications.
- Pair by scanning a short-lived QR code shown on the PC and confirm pairing locally.

## Security requirements
- Pairing begins on the PC; QR challenges are short-lived and single-use, never permanent secrets.
- Use TLS, revocable device identity, expiry, nonces, and replay protection.
- Prefer an outbound PC connection to a relay; no public inbound port or unauthenticated WebSocket.
- Model-provider API keys remain on Windows, never on Android or the relay.
- The PC remains authoritative: mobile requests cannot bypass local approvals or enable Auto-run.
- Require local confirmation for high-risk actions, new-device pairing, screen sharing, and policy changes.
- Provide device list, last-seen status, revocation, session expiry, audit history, and emergency stop.
- Store mobile keys in Android Keystore; never log transcripts, auth tokens, or private payloads.
- Push-to-talk by default; clearly show recording state and provide transcript deletion controls.
- Screen previews are off by default and require per-session consent and appropriate redaction.

## Proposed implementation
- **Android:** Kotlin + Jetpack Compose; speech recognition behind an adapter; encrypted local session state; push notifications.
- **Windows:** shared TaskController, task API, pairing UI, task-state stream, approval broker, and policy engine.
- **Relay (optional):** authenticated outbound connection; minimum routing/task metadata; no screenshots by default.
- **Protocol:** versioned, validated messages with task/client IDs, expiry, nonce, risk classification, status, and structured errors.

## Rollout
1. Build a local-only task API and mock client first.
2. Add secure pairing, device revocation, and authenticated task/status messages.
3. Add Android transcript review, task submission, progress, and Stop.
4. Add voice input and notifications.
5. Add optional screen preview only after consent, redaction, authorization, and retention controls.
6. Threat-model and test pairing, token theft, replay, relay compromise, prompt injection, and lost phones before public remote access.

For the shared remote-control architecture, WhatsApp integration, protocol envelope, webhook security, rollout gates, and official vendor references, see [REMOTE_CONTROL_ARCHITECTURE.md](REMOTE_CONTROL_ARCHITECTURE.md).

## Out of scope for the first mobile release
- No always-on microphone.
- No raw remote keyboard/mouse commands or arbitrary shell endpoint.
- No API keys stored on Android.
- No unattended high-risk actions.
- No public inbound desktop port.
