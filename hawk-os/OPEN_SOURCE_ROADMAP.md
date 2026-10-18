# Hawk open-source roadmap

Hawk is being developed as an open-source desktop AI agent: a local Windows executor that can perceive applications, plan multi-step tasks, ask for approval, execute actions, and verify outcomes. It is not an operating system.

## What should make Hawk useful

- A transparent plan and action timeline instead of a black-box “agent is working” spinner.
- User-controlled model providers; credentials stay on the user's PC.
- Workspace-scoped file tools and visible permissions.
- Stop/pause, checkpoints, recovery, and evidence-based completion.
- UI Automation plus visual perception for application workflows.
- An Android voice companion that submits tasks to the paired PC without bypassing local security controls.

## Open-source essentials

- The repository license is MIT. Review and change it before the first public release if a different licensing model is intended.
- Use GitHub issues for bugs and feature requests; use private security advisories for vulnerabilities.
- Keep secrets, logs, screenshots, executables, debug symbols, and generated caches out of Git.
- Publish reproducible Windows builds from CI, checksums, release notes, and signed binaries once release signing is set up.
- Keep contribution, security, and architecture docs current.
- Add issue templates and a pull request template before inviting broad contributions.

## Release gates

Do not label a release production-ready until it has:
- A clean Windows build and passing offline regression tests.
- An explicit permissions model and adversarial prompt-injection tests.
- Verified cancellation, bounded network timeouts, and crash recovery.
- A clear privacy notice and retention/deletion controls.
- A threat model for any remote or mobile-control channel.

The Android companion is a planned follow-on, not part of the current desktop release.
