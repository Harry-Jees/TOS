"""Configuration for SandboxOS.

All host-side paths are resolved from this file's location, so the whole
project stays portable and the sandbox never depends on the working directory.
"""

from __future__ import annotations

import os
import pathlib

BASE_DIR = pathlib.Path(__file__).resolve().parent.parent  # .../sandbox-os
ROOT_DIR = BASE_DIR / "root"      # the sandbox's virtual disk (its "/")
VAR_DIR = BASE_DIR / "var"        # host-side runtime state: audit log, screenshots
SHOTS_DIR = VAR_DIR / "shots"
WEB_DIR = BASE_DIR / "web"
ENV_FILE = BASE_DIR / ".env"

# --- virtual screen -------------------------------------------------------
SCREEN_W = 960
SCREEN_H = 600
PANEL_H = 22
TASKBAR_H = 26

# --- sandbox resource limits (enforced by the kernel, not by the host) ----
FS_QUOTA_BYTES = 8 * 1024 * 1024      # total bytes the virtual disk may store
FS_MAX_FILE_BYTES = 1024 * 1024       # largest single file
FS_MAX_ENTRIES = 2000                 # inode budget

# --- agent -----------------------------------------------------------------
AGENT_MAX_STEPS = 40
AGENT_MAX_SECONDS = 900
AGENT_MAX_TOOL_RESULT = 6000

# Default model endpoint. NVIDIA NIM is OpenAI-compatible.
DEFAULT_BASE_URL = "https://integrate.api.nvidia.com/v1"
DEFAULT_MODEL = "meta/llama-3.3-70b-instruct"

ENV_KEY_NAMES = ("SANDBOX_LLM_API_KEY", "NVIDIA_API_KEY", "OPENAI_API_KEY")


def load_env(path: pathlib.Path | None = None) -> dict[str, str]:
    """Minimal .env reader (KEY=VALUE, # comments). Existing env wins."""
    path = path or ENV_FILE
    values: dict[str, str] = {}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return values
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.strip().strip('"').strip("'")
        if key:
            values[key] = val
    return values


def get(key: str, default: str | None = None) -> str | None:
    """Environment lookup that also consults the project .env file."""
    if key in os.environ and os.environ[key]:
        return os.environ[key]
    return load_env().get(key, default)


def api_key() -> str | None:
    for name in ENV_KEY_NAMES:
        val = get(name)
        if val:
            return val
    return None


def base_url() -> str:
    return get("SANDBOX_LLM_BASE_URL", DEFAULT_BASE_URL) or DEFAULT_BASE_URL


def model() -> str:
    return get("SANDBOX_LLM_MODEL", DEFAULT_MODEL) or DEFAULT_MODEL


def ensure_dirs() -> None:
    for d in (ROOT_DIR, VAR_DIR, SHOTS_DIR):
        d.mkdir(parents=True, exist_ok=True)
