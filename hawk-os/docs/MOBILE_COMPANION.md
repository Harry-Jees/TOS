# Hawk Android companion plan

## Experience

The Android app should be a remote control and task companion for the Windows Hawk agent:

- Hold-to-talk or tap-to-record voice instruction.
- Show the recognized transcript before submission, with edit/cancel.
- Display the desktop's current state, proposed plan, approval requests, progress, and final result.
- Send follow-up instructions, pause/stop tasks, and receive completion/error notifications.
- Pair by scanning a short-lived QR code displayed by Hawk on the Windows PC.

Example: “Open my editor, create a new notes file in the Hawk workspace, write this outline, and show me the result.” The phone submits the task; the PC reviews policy and performs the actions.

## Security model

Do **not** expose unrestricted mouse/keyboard control through a public port or unauthenticated WebSocket.

1. Pair only when the user initiates it on the PC. QR payload contains a short-lived one-time pairing challenge, not a permanent secret.
2. Authenticate each device with a revocable device key. Use TLS and authenticated messages with nonces, timestamps, and replay protection.
3. Prefer an outbound PC connection to a relay so users do not need router port-forwarding. Relay stores only minimal routing/task metadata and short-lived encrypted payloads.
4. Never put model-provider API keys on the phone or relay. The desktop owns model credentials and the execution policy.
5. The PC remains authoritative: mobile requests cannot bypass the local approval gate or enable Auto-run remotely.
6. Require PC confirmation for high-risk actions, new-device pairing, sensitive screen sharing, and policy changes.
7. Include device list, last-seen time, revoke button, session expiry, audit history, and emergency stop.
8. Default to push-to-talk; do not continuously record ambient audio. Show mic/recording state clearly and let users delete transcripts.
9. Screenshots stay on the PC unless the user explicitly requests a preview and consents to transmission.
10. Use Android Keystore for local device keys; never log transcripts, auth tokens, or private payloads.

## Proposed components

- **Android**: Kotlin + Jetpack Compose, Android SpeechRecognizer or an explicitly chosen speech-to-text provider, encrypted local token storage, push notifications.
- **Windows**: local task API, pairing UI, task state stream, approval broker, policy engine.
- **Relay (optional)**: authenticated outbound WebSocket relay for presence and task messages. It routes encrypted messages and does not execute actions or store screenshots by default.
- **Protocol**: versioned JSON messages with task IDs, device IDs, nonce, expiry, action risk, status, and structured errors. Validate every field and cap payload sizes.

## Rollout

1. Ship a local-only Windows API and pairing UI with mock clients.
2. Implement secure pairing, device revocation, and authenticated task/status messages.
3. Add Android transcript preview, task submission, progress, and Stop.
4. Add voice input and notifications.
5. Add optional remote screen preview only after explicit per-session consent and redaction controls.
6. Threat-model and penetration-test pairing, token theft, replay, relay compromise, malicious prompts, and lost phones before public remote access.

## Deliberately out of scope for the first mobile version

- No always-on microphone.
- No direct phone-to-PC raw keyboard/mouse commands.
- No API keys stored on Android.
- No unattended high-risk actions.
- No public inbound desktop port.
