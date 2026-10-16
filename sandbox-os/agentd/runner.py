"""The agent loop: think -> act -> observe, inside the sandbox, with a budget."""

from __future__ import annotations

import json
import threading
import time

from sandboxos import config

from . import prompts
from .providers import LLMProvider, ProviderError, ScriptedProvider, ToolsUnsupported, build_provider
from .tools import TERMINAL_TOOLS, TOOL_SCHEMAS, Toolbox


class AgentRunner:
    def __init__(
        self,
        kernel,
        provider: LLMProvider | None = None,
        max_steps: int | None = None,
        max_seconds: int | None = None,
        protocol: str = "auto",
    ):
        self.kernel = kernel
        self.toolbox = Toolbox(kernel)
        self.provider = provider or build_provider(kernel)
        self.max_steps = max_steps or int(config.get("SANDBOX_AGENT_MAX_STEPS", "") or config.AGENT_MAX_STEPS)
        self.max_seconds = max_seconds or config.AGENT_MAX_SECONDS
        self.protocol = protocol if protocol in ("auto", "native", "json") else "auto"
        self._abort = threading.Event()
        self._thread: threading.Thread | None = None
        self.last_summary: str | None = None
        self.history: list[dict] = []
        self.kernel.agent = self

    # -- public API --------------------------------------------------------
    def submit(self, task: str, background: bool = True) -> None:
        """Give the agent a task. Background by default so HTTP stays responsive."""
        if self.is_running:
            self.kernel.notify("agent is already working - abort first", level="error")
            return
        self._abort.clear()
        if background:
            self._thread = threading.Thread(target=self._run_guarded, args=(task,), daemon=True)
            self._thread.start()
        else:
            self._run_guarded(task)

    @property
    def is_running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def abort(self) -> None:
        self._abort.set()
        self.kernel.emit("system", "abort requested")
        if self._thread is not None and self._thread.is_alive():
            self._thread.join(timeout=2)

    # -- loop --------------------------------------------------------------
    def _run_guarded(self, task: str) -> None:
        try:
            self.run(task)
        except Exception as exc:  # never take the machine down with us
            self.kernel.emit("error", f"agent crashed: {type(exc).__name__}: {exc}")
            self.kernel.agent_state.update({"state": "failed"})

    def run(self, task: str) -> str:
        self.kernel.agent_state.update(
            {"state": "running", "steps": 0, "task": task, "started": time.time()}
        )
        self.kernel.emit("agent", f"task: {task}")
        self.kernel.audit.record("agent_task_start", actor="agent", task=task,
                                 model=getattr(self.provider, "description", self.provider.name))
        messages: list[dict] = [
            {"role": "system", "content": self._system_prompt()},
            {"role": "user", "content": task},
        ]
        start = time.time()
        used_json = self.protocol == "json"
        summary: str | None = None

        for step in range(1, self.max_steps + 1):
            if self._abort.is_set():
                summary = "Aborted at your request."
                self.kernel.emit("system", summary)
                break
            if time.time() - start > self.max_seconds:
                summary = f"Stopped: {self.max_seconds}s time budget exhausted."
                self.kernel.emit("system", summary)
                break

            self.kernel.agent_state["steps"] = step
            try:
                if used_json:
                    content, calls = self._chat_json(messages)
                else:
                    resp = self.provider.chat(messages, TOOL_SCHEMAS if self.provider.supports_tools else None)
                    content, calls = resp.get("content"), resp.get("tool_calls") or []
            except ToolsUnsupported as exc:
                self.kernel.emit("system", f"endpoint rejected native tools ({str(exc)[:120]}); switching to JSON protocol")
                used_json = True
                content, calls = self._chat_json(messages)
            except ProviderError as exc:
                self.kernel.emit("error", f"model error: {exc}")
                summary = f"Stopped: model error: {exc}"
                break

            if content:
                self.kernel.emit("thought", str(content).strip()[:600])

            if not calls:
                summary = (content or "").strip() or "Finished with no summary."
                self.kernel.emit("agent", "task finished")
                break

            messages.append(self._assistant_message(content, calls, used_json))
            finished = False
            for call in calls:
                name, args = call["name"], call.get("arguments") or {}
                self.kernel.emit("tool", f"{name}({json.dumps(args, default=str)[:200]})")
                ok, result = self.toolbox.execute(name, args)
                self.kernel.emit("result" if ok else "error", f"{name} -> {result[:400]}")
                self.kernel.audit.record(
                    "agent_tool", actor="agent", tool=name, ok=ok,
                    arguments=args, result=result[:600],
                )
                if used_json:
                    messages.append({"role": "user", "content": f"TOOL RESULT ({name}): {result}"})
                else:
                    messages.append(
                        {"role": "tool", "tool_call_id": call["id"], "name": name, "content": result}
                    )
                if name in TERMINAL_TOOLS:
                    summary = str(args.get("summary") or result)
                    finished = True
            if finished:
                break
        else:
            summary = f"Reached the {self.max_steps} step budget without finishing."

        summary = summary or "Finished."
        self.last_summary = summary
        self.history.append({"task": task, "summary": summary})
        self.kernel.agent_state.update({"state": "idle", "task": None})
        self.kernel.emit("agent", f"summary: {summary[:400]}")
        self.kernel.audit.record("agent_task_end", actor="agent", task=task,
                                 summary=summary[:600], steps=self.kernel.agent_state["steps"])
        return summary

    # -- protocol helpers --------------------------------------------------
    def _system_prompt(self) -> str:
        scr = self.kernel.screen
        extra = ""
        if self.protocol == "json":
            tool_list = "\n".join(
                f"- {t['function']['name']}: {t['function']['description']} "
                f"args={json.dumps(t['function']['parameters']['properties'])}"
                for t in TOOL_SCHEMAS
            )
            extra = prompts.JSON_PROTOCOL.format(tools=tool_list)
        return prompts.build_system_prompt(scr.width, scr.height, extra)

    def _assistant_message(self, content, calls, used_json: bool) -> dict:
        if used_json:
            return {"role": "assistant", "content": json.dumps(
                {"thought": content, "tool": calls[0]["name"], "arguments": calls[0]["arguments"]})}
        return {
            "role": "assistant",
            "content": content or "",
            "tool_calls": [
                {
                    "id": c["id"],
                    "type": "function",
                    "function": {"name": c["name"], "arguments": json.dumps(c.get("arguments") or {})},
                }
                for c in calls
            ],
        }

    def _chat_json(self, messages: list[dict]) -> tuple[str | None, list[dict]]:
        """Text-only fallback: ask for one JSON action per reply."""
        resp = self.provider.chat(messages, tools=None)
        text = (resp.get("content") or "").strip()
        obj = _extract_json(text)
        if not obj:
            return text, []
        if obj.get("final"):
            return str(obj.get("final")), []
        tool = obj.get("tool")
        if not tool:
            return obj.get("thought") or text, []
        args = obj.get("arguments") or {}
        if not isinstance(args, dict):
            args = {"_value": args}
        return str(obj.get("thought") or ""), [
            {"id": f"json_{len(messages)}", "name": str(tool), "arguments": args}
        ]


def _extract_json(text: str) -> dict | None:
    """Pull the first balanced JSON object out of a model reply."""
    depth = 0
    start = -1
    in_str = False
    escape = False
    for i, ch in enumerate(text):
        if in_str:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
        elif ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start >= 0:
                try:
                    return json.loads(text[start : i + 1])
                except json.JSONDecodeError:
                    start = -1
    return None


def make_runner(kernel, provider: LLMProvider | None = None) -> AgentRunner:
    """Used by tests and the CLI to get a runner with a predictable brain."""
    if provider is None:
        provider = ScriptedProvider()
    return AgentRunner(kernel, provider=provider)
