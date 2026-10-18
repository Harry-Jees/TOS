# Contributing to Hawk

Thanks for helping improve Hawk. The project is a Windows desktop agent that perceives the screen, plans tasks, and operates the real desktop.

## Before submitting a change

1. Open or comment on an issue for larger changes so the architecture and safety implications can be discussed.
2. Keep changes focused. Separate UI work, action-runtime changes, model-provider changes, and documentation where possible.
3. Never commit API keys, personal screenshots, logs, `hawk.ini`, generated executables, debug symbols, or toolchain caches.
4. For changes that can type, click, open, close, create, or delete, document the failure mode and add a regression test.
5. Prefer explicit typed outcomes and observable postconditions over assuming an action succeeded.

## Local build

Use a Windows environment with a C++17 compiler and Windows resource compiler:

```bash
cd hawk-os
bash build.sh
./hawk.exe --help
```

Run the offline goal-loop regression scenarios after building:

```bash
python tools/verify_goal_loop.py
```

The interactive tests in `tests/test_hawk.py` and `tests/test_gui.py` move the real cursor and interact with a real desktop. Run them only in a disposable, visible Windows session where that is safe.

## Pull request checklist

- [ ] Explain the user problem and behavior change.
- [ ] State which tests were run and which were not.
- [ ] Include screenshots for meaningful UI changes, with private information removed.
- [ ] Consider prompt injection, destructive actions, secrets, cancellation, and partial failure.
- [ ] Update README or architecture docs if behavior or security boundaries change.
