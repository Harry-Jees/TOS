"""The agent: real multi-step work inside the sandbox, with guardrails."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from agentd import AgentRunner, ScriptedProvider, build_provider  # noqa: E402
from agentd.runner import _extract_json  # noqa: E402
from agentd.tools import TOOL_SCHEMAS, Toolbox  # noqa: E402
from sandboxos import config  # noqa: E402
from sandboxos.kernel import Kernel  # noqa: E402


class AgentCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self._tmp.name)
        self.shots = base / "shots"
        self.shots.mkdir()
        self._old_shots = config.SHOTS_DIR
        config.SHOTS_DIR = self.shots  # keep test screenshots out of the project
        self.kernel = Kernel(root=base / "disk", var_dir=base / "var", boot_windows=True)
        self.runner = AgentRunner(self.kernel, provider=ScriptedProvider())

    def tearDown(self):
        config.SHOTS_DIR = self._old_shots
        self._tmp.cleanup()


class TestScriptedAgent(AgentCase):
    def test_full_task_creates_file_runs_shell_and_screenshots(self):
        summary = self.runner.run("write a report and prove it on screen")
        self.assertTrue(summary)
        self.assertTrue(self.kernel.vfs.exists("/home/user/notes/agent-report.md"))
        report = self.kernel.vfs.read_text("/home/user/notes/agent-report.md")
        self.assertIn("Agent report", report)
        self.assertIn("inside SandboxOS", report)
        self.assertTrue(list(self.shots.glob("*.png")), "agent should have saved a screenshot")
        self.assertGreaterEqual(self.kernel.agent_state["steps"], 3)
        self.assertEqual(self.kernel.agent_state["state"], "idle")

    def test_agent_types_into_the_on_screen_terminal(self):
        self.runner.run("type into the terminal")
        term = next(w for w in self.kernel.screen.windows if w.app == "terminal")
        transcript = "\n".join(t for t, _ in term.lines)
        self.assertIn("echo written by the agent", transcript)

    def test_every_tool_call_is_audited(self):
        self.runner.run("do the thing")
        entries = self.kernel.audit.tail(200)
        tools = [e for e in entries if e["action"] == "agent_tool"]
        self.assertGreaterEqual(len(tools), 5)
        self.assertTrue(all(t["actor"] == "agent" for t in tools))
        self.assertTrue(any(e["action"] == "agent_task_end" for e in entries))

    def test_events_stream_for_the_ui(self):
        self.runner.run("stream events")
        kinds = {e["kind"] for e in self.kernel.events_since(0)}
        self.assertTrue({"agent", "tool", "result"} <= kinds)
        self.assertTrue(any(e["kind"] == "screenshot" for e in self.kernel.events))

    def test_step_budget_is_enforced(self):
        runner = AgentRunner(self.kernel, provider=ScriptedProvider(), max_steps=3)
        summary = runner.run("short budget")
        self.assertIn("budget", summary.lower())
        self.assertLessEqual(self.kernel.agent_state["steps"], 3)

    def test_abort_stops_the_loop(self):
        runner = AgentRunner(self.kernel, provider=ScriptedProvider(), max_steps=50)
        runner._abort.set()
        summary = runner.run("aborted task")
        self.assertIn("abort", summary.lower())


class TestToolboxGuardrails(AgentCase):
    def setUp(self):
        super().setUp()
        self.tools = Toolbox(self.kernel)

    def test_schemas_are_well_formed(self):
        names = [t["function"]["name"] for t in TOOL_SCHEMAS]
        self.assertIn("shell_run", names)
        self.assertIn("screen_click", names)
        self.assertEqual(len(names), len(set(names)), "duplicate tool names")
        for schema in TOOL_SCHEMAS:
            self.assertEqual(schema["type"], "function")
            params = schema["function"]["parameters"]
            self.assertEqual(params["type"], "object")
            for req in params.get("required", []):
                self.assertIn(req, params["properties"])

    def test_out_of_bounds_click_is_refused(self):
        ok, result = self.tools.execute("screen_click", {"x": 5000, "y": 10})
        self.assertFalse(ok)
        self.assertIn("outside the screen", result)

    def test_missing_file_is_an_error_not_a_crash(self):
        ok, result = self.tools.execute("fs_read", {"path": "/nope.txt"})
        self.assertFalse(ok)
        self.assertIn("failed", result)

    def test_unknown_tool(self):
        ok, result = self.tools.execute("format_c_drive", {})
        self.assertFalse(ok)
        self.assertIn("unknown tool", result)

    def test_bad_arguments_do_not_crash(self):
        ok, result = self.tools.execute("fs_write", {"path": "/x.txt"})
        self.assertFalse(ok)
        self.assertIn("bad arguments", result)

    def test_window_focus_by_app(self):
        ok, result = self.tools.execute("window_focus", {"app": "files"})
        self.assertTrue(ok, result)
        self.assertEqual(self.kernel.screen.focus.app, "files")

    def test_window_focus_opens_a_closed_app(self):
        """A focus on a closed app must not strand the agent (taskbar behaviour)."""
        self.kernel.close_all_windows()
        self.assertEqual(len(self.kernel.screen.windows), 0)
        ok, result = self.tools.execute("window_focus", {"app": "terminal"})
        self.assertTrue(ok, result)
        self.assertEqual(self.kernel.screen.focus.app, "terminal")
        # and typing now lands in the terminal instead of nowhere
        ok, result = self.tools.execute("screen_type", {"text": "echo recovered"})
        self.assertTrue(ok)
        self.assertIn("user@sandbox", result)  # the terminal's window title

    def test_window_focus_by_bad_id_still_fails(self):
        ok, result = self.tools.execute("window_focus", {"id": 9999})
        self.assertFalse(ok)
        self.assertIn("no window with id", result)

    def test_scripted_plan_survives_a_missing_terminal(self):
        """The default demo brain must still drive the terminal on a bare desktop."""
        self.kernel.close_all_windows()
        runner = AgentRunner(self.kernel, provider=ScriptedProvider())
        runner.run("drive the terminal with no windows open")
        term = next((w for w in self.kernel.screen.windows if w.app == "terminal"), None)
        self.assertIsNotNone(term, "the scripted agent should have opened a terminal")
        transcript = "\n".join(t for t, _ in term.lines)
        self.assertIn("echo written by the agent", transcript)

    def test_shell_tool_has_no_host_binaries(self):
        ok, result = self.tools.execute("shell_run", {"command": "bash -c whoami"})
        self.assertTrue(ok)  # the command ran; the shell just refused it
        self.assertIn("127", result)
        self.assertIn("command not found", result)

    def test_filesystem_tool_stays_in_the_jail(self):
        ok, result = self.tools.execute("fs_write", {"path": "/../../escape.txt", "content": "x"})
        self.assertFalse(ok)
        self.assertIn("failed", result)
        self.assertFalse((self.kernel.root.parent / "escape.txt").exists())

    def test_screenshot_tool_returns_a_url(self):
        ok, result = self.tools.execute("screen_screenshot", {"label": "unit test"})
        self.assertTrue(ok, result)
        self.assertIn("/shots/", result)
        self.assertTrue(list(self.shots.glob("*.png")))

    def test_task_complete_is_terminal(self):
        ok, result = self.tools.execute("task_complete", {"summary": "done"})
        self.assertTrue(ok)
        self.assertIn("done", result)


class TestProviderSelection(unittest.TestCase):
    def test_falls_back_to_scripted_without_a_key(self):
        kernel = Kernel(root=pathlib.Path(tempfile.mkdtemp()) / "d",
                        var_dir=pathlib.Path(tempfile.mkdtemp()), boot_windows=False)
        provider = build_provider(kernel, force=None)
        # No key is configured in the test environment, so we get the scripted brain
        if config.api_key() is None:
            self.assertEqual(provider.name, "scripted")
        provider = build_provider(kernel, force="scripted")
        self.assertEqual(provider.name, "scripted")

    def test_scripted_plan_is_deterministic(self):
        p = ScriptedProvider()
        first = p.chat([{"role": "user", "content": "x"}])
        second = p.chat([{"role": "user", "content": "x"}])
        self.assertEqual(first["tool_calls"][0]["name"], second["tool_calls"][0]["name"])

    def test_json_protocol_extraction(self):
        self.assertEqual(_extract_json('{"tool": "shell_run", "arguments": {"command": "ls"}}')["tool"], "shell_run")
        self.assertIsNone(_extract_json("no json here"))
        # nested braces and strings
        parsed = _extract_json('prose {"tool":"fs_write","arguments":{"content":"a { } b"}} trailing')
        self.assertEqual(parsed["arguments"]["content"], "a { } b")


if __name__ == "__main__":
    unittest.main()
