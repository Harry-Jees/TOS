# Hawk OS

**Hawk is a Windows desktop AI agent that can see the screen, operate real apps, and verify multi-step tasks.** Despite the name, it is a desktop application—not an operating system.

The project is evolving toward an open-source, cowork-style desktop agent: users describe a task, inspect the plan, approve risky steps, follow execution, and review the result. A future Android companion will let users submit voice instructions to their paired Windows PC.

## Project status

This is an actively developed prototype, not a production-ready autonomous computer controller. Hawk can inject real mouse/keyboard input and send screenshots to a user-configured model provider. Use it only on a desktop and data you control.

## Start here

- [Hawk desktop app: capabilities, build, and CLI](hawk-os/README.md)
- [Architecture and security-hardening roadmap](hawk-os/docs/ARCHITECTURE.md)
- [Android voice companion plan](hawk-os/docs/MOBILE_COMPANION.md)
- [Open-source roadmap](hawk-os/OPEN_SOURCE_ROADMAP.md)
- [Security policy](SECURITY.md)
- [Contributing guide](CONTRIBUTING.md)
- [MIT License](LICENSE)

## Safety defaults

- Model-generated scripts do **not** execute automatically by default.
- The app warns before sending a screen capture to the configured model provider.
- File commands are confined to Hawk's workspace and reject path traversal.
- New file writes refuse to overwrite an existing workspace file.
- Saved model API keys use Windows DPAPI when saved through Model settings.
- These protections are not a sandbox; desktop actions can still affect signed-in applications.

## Build

On Windows with a C++17 toolchain and Windows resource compiler:

```bat
cd hawk-os
build.bat
```

Or from a compatible Bash environment:

```bash
cd hawk-os
./build.sh
```

GitHub Actions builds Hawk and runs non-interactive safety and offline goal-loop regression tests. Interactive GUI tests require a disposable visible Windows session and are not run in hosted CI.

## Mobile companion direction

The Android app is planned as a secure remote task companion, not a direct unauthenticated mouse/keyboard tunnel. Pairing, device revocation, authenticated messages, replay protection, explicit approvals, and an emergency stop are required before remote control is exposed.

## Contributing

Bug reports, focused pull requests, architecture discussion, and safety tests are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and report vulnerabilities privately using the GitHub Security Advisories feature.
