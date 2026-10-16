"""Model providers for agentd.

`OpenAICompatProvider` speaks the OpenAI chat-completions protocol over plain
HTTP (stdlib only), which is what NVIDIA NIM, OpenAI, OpenRouter, Groq,
DeepSeek and most local servers expose. `ScriptedProvider` is a deterministic
brain that performs a real multi-step task without any network, so the sandbox
is usable - and testable - before a key is added.
"""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request

from sandboxos import config


class ProviderError(Exception):
    """Any failure while talking to the model."""


class ToolsUnsupported(ProviderError):
    """The model/endpoint rejected native tool calling."""


class LLMProvider:
    name = "provider"
    supports_tools = True
    description = "base provider"

    def chat(self, messages: list[dict], tools: list[dict] | None = None) -> dict:
        raise NotImplementedError

    # convenience used by the runner
    def complete_text(self, messages: list[dict]) -> str:
        resp = self.chat(messages, tools=None)
        return resp.get("content") or ""


class OpenAICompatProvider(LLMProvider):
    name = "openai-compatible"

    def __init__(
        self,
        api_key: str,
        base_url: str | None = None,
        model: str | None = None,
        timeout: int = 240,
        max_tokens: int = 2048,
        temperature: float = 0.2,
        max_retries: int = 3,
    ):
        if not api_key:
            raise ProviderError("no API key configured")
        self.api_key = api_key
        self.base_url = (base_url or config.base_url()).rstrip("/")
        self.model = model or config.model()
        self.timeout = timeout
        self.max_tokens = max_tokens
        self.temperature = temperature
        self.max_retries = max_retries
        self.description = f"{self.model} via {self.base_url}"

    # -- HTTP --------------------------------------------------------------
    def _post(self, payload: dict) -> dict:
        url = f"{self.base_url}/chat/completions"
        body = json.dumps(payload).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json",
            "Authorization": f"Bearer {self.api_key}",
            "User-Agent": "SandboxOS/1.0 (+agentd)",
        }
        last_error: Exception | None = None
        for attempt in range(self.max_retries):
            req = urllib.request.Request(url, data=body, headers=headers, method="POST")
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    return json.loads(resp.read().decode("utf-8", errors="replace"))
            except urllib.error.HTTPError as exc:
                detail = ""
                try:
                    detail = exc.read().decode("utf-8", errors="replace")[:600]
                except Exception:
                    pass
                if exc.code in (400, 404, 422) and "tool" in detail.lower():
                    raise ToolsUnsupported(f"HTTP {exc.code}: {detail}") from exc
                if exc.code in (401, 403):
                    raise ProviderError(
                        f"HTTP {exc.code} from {url}: check your API key ({detail[:200]})"
                    ) from exc
                last_error = ProviderError(f"HTTP {exc.code}: {detail[:300]}")
                if exc.code in (429, 500, 502, 503, 504) and attempt < self.max_retries - 1:
                    time.sleep(1.5 * (attempt + 1))
                    continue
                raise last_error from exc
            except (urllib.error.URLError, TimeoutError, OSError) as exc:
                last_error = ProviderError(f"network error talking to {url}: {exc}")
                if attempt < self.max_retries - 1:
                    time.sleep(1.5 * (attempt + 1))
                    continue
                raise last_error from exc
            except json.JSONDecodeError as exc:
                raise ProviderError(f"model endpoint returned non-JSON: {exc}") from exc
        raise last_error or ProviderError("request failed")

    # -- protocol ----------------------------------------------------------
    def chat(self, messages: list[dict], tools: list[dict] | None = None) -> dict:
        payload = {
            "model": self.model,
            "messages": messages,
            "temperature": self.temperature,
            "max_tokens": self.max_tokens,
            "stream": False,
        }
        if tools:
            payload["tools"] = tools
            payload["tool_choice"] = "auto"
        data = self._post(payload)
        try:
            choice = data["choices"][0]
            msg = choice.get("message") or {}
        except (KeyError, IndexError, TypeError) as exc:
            raise ProviderError(f"unexpected response shape: {str(data)[:300]}") from exc

        calls = []
        for call in msg.get("tool_calls") or []:
            fn = call.get("function") or {}
            raw_args = fn.get("arguments") or "{}"
            try:
                args = json.loads(raw_args) if isinstance(raw_args, str) else (raw_args or {})
            except json.JSONDecodeError:
                args = {"_raw": raw_args}
            calls.append(
                {
                    "id": call.get("id") or f"call_{len(calls)}",
                    "name": fn.get("name") or "",
                    "arguments": args if isinstance(args, dict) else {"_value": args},
                }
            )
        return {
            "content": msg.get("content"),
            "tool_calls": calls,
            "finish_reason": choice.get("finish_reason"),
            "usage": data.get("usage"),
            "raw": msg,
        }


class ScriptedProvider(LLMProvider):
    """A deterministic agent used for demos, offline use and tests.

    It performs a genuine multi-step task against the sandbox: inspect the
    machine, read a file, write a report, run shell commands, drive the screen
    and take screenshots.
    """

    name = "scripted"
    description = "built-in script (no model required)"

    PLAN = [
        ("shell_run", {"command": "uname -a && df -h /"}),
        ("fs_list", {"path": "/home/user"}),
        ("fs_read", {"path": "/home/user/notes/todo.md"}),
        ("fs_search", {"pattern": "agent", "path": "/"}),
        (
            "fs_write",
            {
                "path": "/home/user/notes/agent-report.md",
                "content": (
                    "# Agent report\n\n"
                    "I am an AI agent running inside SandboxOS. This file was written\n"
                    "through the sandbox filesystem tool, then verified with the shell.\n\n"
                    "- inspected the machine with `uname -a`\n"
                    "- listed /home/user and read notes/todo.md\n"
                    "- searched the disk for the word 'agent'\n"
                    "- wrote this report, then typed into the on-screen terminal\n"
                    "- captured screenshots of the virtual screen\n"
                ),
            },
        ),
        ("shell_run", {"command": "cat /home/user/notes/agent-report.md"}),
        ("window_focus", {"app": "terminal"}),
        ("screen_type", {"text": "echo written by the agent && ls -l /home/user/notes"}),
        ("screen_key", {"key": "Enter"}),
        ("screen_describe", {}),
        ("screen_screenshot", {"label": "agent-finished"}),
    ]

    def chat(self, messages: list[dict], tools: list[dict] | None = None) -> dict:
        done = sum(1 for m in messages if m.get("role") == "tool")
        if done < len(self.PLAN):
            name, args = self.PLAN[done]
            return {
                "content": f"step {done + 1}/{len(self.PLAN)}: {name} "
                           f"{json.dumps(args)[:160]}",
                "tool_calls": [
                    {"id": f"scripted_{done}", "name": name, "arguments": dict(args)}
                ],
                "finish_reason": "tool_calls",
                "usage": None,
                "raw": {},
            }
        return {
            "content": (
                "Task complete. I wrote /home/user/notes/agent-report.md, verified it "
                "with the sandbox shell, typed a command into the on-screen terminal, "
                "and saved a screenshot. (This run used the built-in scripted brain - "
                "add an NVIDIA_API_KEY to .env for real reasoning.)"
            ),
            "tool_calls": [],
            "finish_reason": "stop",
            "usage": None,
            "raw": {},
        }


def build_provider(kernel=None, force: str | None = None, model: str | None = None):
    """Pick a provider: scripted when asked or when no key exists."""
    mode = (force or config.get("SANDBOX_AGENT", "") or "").strip().lower()
    key = config.api_key()
    if mode in ("scripted", "demo", "offline"):
        return ScriptedProvider()
    if not key:
        if kernel is not None:
            kernel.emit(
                "system",
                "no SANDBOX_LLM_API_KEY / NVIDIA_API_KEY found - using the built-in "
                "scripted brain. Add your key to .env (see .env.example) for real reasoning.",
            )
        return ScriptedProvider()
    try:
        provider = OpenAICompatProvider(key, model=model)
    except ProviderError as exc:
        if kernel is not None:
            kernel.emit("error", f"provider setup failed: {exc}; falling back to script")
        return ScriptedProvider()
    if kernel is not None:
        kernel.emit("system", f"agent brain: {provider.description}")
    return provider
