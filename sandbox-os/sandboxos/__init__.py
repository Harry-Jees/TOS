"""SandboxOS - a self-contained operating system simulated in Python.

The kernel owns a jailed virtual filesystem, a virtual screen with a window
manager, a shell, an event bus and an audit log. An AI agent (or a human at the
browser console) drives all of it through the same interfaces.
"""

from .config import (  # noqa: F401
    AGENT_MAX_STEPS,
    AGENT_MAX_SECONDS,
    BASE_DIR,
    ROOT_DIR,
    SCREEN_H,
    SCREEN_W,
    SHOTS_DIR,
    VAR_DIR,
)
from .kernel import Kernel  # noqa: F401
from .screen import APP_CLASSES  # noqa: F401
from .shell import ShellSession, run_line  # noqa: F401
from .vfs import Vfs, VfsError  # noqa: F401

__version__ = "1.0.0"
__all__ = [
    "Kernel",
    "Vfs",
    "VfsError",
    "ShellSession",
    "run_line",
    "APP_CLASSES",
    "__version__",
]
