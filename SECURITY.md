# Security policy

Hawk can control the real Windows desktop. Treat it as privileged automation software, not a harmless chatbot.

## Screenshot and credential privacy

The desktop UI asks before sending screenshots to the configured model provider. CLI users must explicitly pass `--allow-screen-upload` for image-bearing model requests. Do not use that option with untrusted scripts or endpoints. Non-loopback API endpoints must use HTTPS. Saved API keys use Windows DPAPI for the current Windows user; legacy plaintext settings may still be read for migration, so resave settings after upgrading.

## Reporting a vulnerability

Please do not publish exploitable details in a public issue. Contact the repository owner privately through GitHub's security advisory feature (Security → Advisories → Report a vulnerability). Include affected version/commit, reproduction steps, impact, and any safe proof of concept.

## Current safeguards and limitations

- Model-generated goal scripts are not executed automatically by default. Enabling Auto-run or passing `--auto-run` deliberately grants execution permission.
- Goal mode warns before sending a screenshot to the configured model provider.
- The file command language is intended to stay inside Hawk's workspace and rejects absolute paths, profile shortcuts, traversal components, and existing reparse-point paths.
- New `write` operations refuse to overwrite an existing workspace file; deleting a file is an explicit action.
- Semantic UI Automation actions target a uniquely named, visible, enabled control in the selected external app. Ambiguous names fail closed; high-impact labels and credential/payment fields require approval when detected.
- State-changing semantic actions trigger a fresh visual verification round before the goal plan continues. UI Automation success is not proof that the user's overall goal was achieved.
- The settings UI stores API keys using Windows DPAPI for the current Windows user. Legacy plaintext `api_key=` entries remain readable for migration; open Model settings and save to rewrite them in protected form.
- Typed text is no longer copied into Hawk's action log.

These controls reduce risk but are **not a security sandbox**. A permitted desktop action can still click destructive controls, access signed-in sessions, or expose information. Do not enable Auto-run for untrusted tasks or use Hawk unattended on a sensitive desktop.

## Data and privacy

Screenshots can contain messages, documents, credentials, and personal data. Goal mode sends screenshots and task context to the model provider configured by the user. Provider retention and processing policies apply. Local logs and screenshots may also contain sensitive context; protect the Windows account and delete artifacts when no longer needed.

## Supported environment

The current desktop executor targets Windows. Do not expose its control interface to a network or mobile client until pairing, authentication, authorization, replay protection, transport security, revocation, and high-risk action approval are implemented and reviewed.
