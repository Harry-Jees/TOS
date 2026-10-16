"""Desktop app tests.

The Tk window is created hidden (withdrawn) so running the suite never flashes
a window on screen, but the real widgets, the real event pump and the real
input path are exercised.
"""

from __future__ import annotations

import pathlib
import sys
import tempfile
import time
import types
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

try:  # Tk may be missing on a stripped-down Python build
    import tkinter as tk

    _probe = tk.Tk()
    _probe.withdraw()
    _probe.destroy()
    TK_OK = True
except Exception:  # pragma: no cover - depends on the host
    TK_OK = False

from agentd import AgentRunner, ScriptedProvider  # noqa: E402
from sandboxos import config  # noqa: E402
from sandboxos.kernel import Kernel  # noqa: E402


@unittest.skipUnless(TK_OK, "tkinter is not available in this Python build")
class DesktopCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import desktop

        cls.desktop = desktop
        cls._tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls._tmp.name)
        cls._old_shots = config.SHOTS_DIR
        config.SHOTS_DIR = base / "shots"
        config.SHOTS_DIR.mkdir()
        cls.kernel = Kernel(root=base / "disk", var_dir=base / "var", boot_windows=True)
        cls.runner = AgentRunner(cls.kernel, provider=ScriptedProvider())
        cls.app = desktop.SandboxDesktop(cls.kernel, cls.runner, scale=1.0)
        cls.app.root.withdraw()  # never show up on the user's screen during tests

    @classmethod
    def tearDownClass(cls):
        try:
            cls.app.quit()
        except Exception:
            pass
        config.SHOTS_DIR = cls._old_shots
        cls._tmp.cleanup()

    def pump(self, seconds: float = 0.5) -> None:
        end = time.time() + seconds
        while time.time() < end:
            self.app.root.update()
            time.sleep(0.01)

    def fake_event(self, x=0, y=0, keysym="", char="", delta=0):
        return types.SimpleNamespace(x=x, y=y, keysym=keysym, char=char, delta=delta)

    def canvas_expected_width(self) -> int:
        """Tk adds the highlight border to the requested canvas size."""
        border = 2 * int(self.app.canvas.cget("highlightthickness"))
        return int(self.kernel.screen.width * self.app.scale) + border

    # -- window / rendering ------------------------------------------------
    def test_window_and_canvas_are_the_right_size(self):
        border = 2 * int(self.app.canvas.cget("highlightthickness"))
        self.assertEqual(self.app.canvas.winfo_reqwidth(), self.kernel.screen.width + border)
        self.assertEqual(self.app.canvas.winfo_reqheight(), self.kernel.screen.height + border)
        self.assertEqual(self.app.root.title().split(" - ")[0], "SandboxOS 1.0")

    def test_screen_renders_into_the_canvas(self):
        self.app._last_seq = -1
        self.app._render_screen()
        self.assertIsNotNone(self.app._photo)
        self.assertEqual(self.app._photo.width(), self.kernel.screen.width)
        self.assertEqual(self.app._photo.height(), self.kernel.screen.height)
        first = self.app._photo
        # nothing changed, so no new image is built (this is what prevents flicker)
        self.app._render_screen()
        self.assertIs(self.app._photo, first)
        # a change does produce a new frame
        self.kernel.screen.touch()
        self.app._render_screen()
        self.assertIsNot(self.app._photo, first)

    def test_scale_change_resizes_canvas_and_image(self):
        self.app.set_scale(0.5)
        try:
            self.assertEqual(self.app.canvas.winfo_reqwidth(), self.canvas_expected_width())
            self.app._render_screen()
            self.assertEqual(self.app._photo.width(), self.kernel.screen.width // 2)
        finally:
            self.app.set_scale(1.0)

    def test_tabs_exist(self):
        names = [self.app.tabs.tab(i, "text") for i in range(len(self.app.tabs.tabs()))]
        self.assertEqual(names, ["Agent", "System", "Screenshots", "Audit"])

    # -- input -------------------------------------------------------------
    def test_click_forwards_to_the_sandbox_screen(self):
        for win in list(self.kernel.screen.windows):
            if win.app == "editor":
                self.kernel.close_window(win.id)
        self.app._mouse("down", self.fake_event(159, 586))
        self.app._mouse("up", self.fake_event(159, 586))
        self.assertTrue(any(w.app == "editor" for w in self.kernel.screen.windows),
                        "clicking the taskbar in the app should open the editor")

    def test_typing_forwarding_runs_a_command(self):
        term = next(w for w in self.kernel.screen.windows if w.app == "terminal")
        self.kernel.focus_window(term.id)
        for ch in "echo desktop-app":
            self.app._key(self.fake_event(keysym=ch, char=ch))
        self.app._key(self.fake_event(keysym="Return", char="\r"))
        transcript = "\n".join(t for t, _ in term.lines)
        self.assertIn("echo desktop-app", transcript)
        self.assertIn("desktop-app", transcript.split("echo desktop-app")[-1])

    def test_mouse_motion_tracks_the_cursor(self):
        self.app._mouse("move", self.fake_event(100, 120))
        self.assertEqual(tuple(self.kernel.screen.cursor), (100, 120))

    def test_wheel_scrolls_without_error(self):
        self.app._wheel(self.fake_event(200, 150, delta=120))
        self.app._wheel(self.fake_event(200, 150, delta=-120))

    # -- event pump / feed -------------------------------------------------
    def test_feed_shows_events_and_shell_output(self):
        self.app.shell_var.set("echo from-the-desktop-shell")
        self.app.run_shell()
        self.app._pump_events()
        text = self.app.feed.get("1.0", "end")
        self.assertIn("from-the-desktop-shell", text)
        self.assertIn("boot complete", text)

    def test_status_bar_reports_machine_state(self):
        self.app._update_status()
        status = self.app.status.cget("text")
        self.assertIn("agent:", status)
        self.assertIn("windows:", status)
        self.assertIn("disk:", status)

    def test_shots_tab_picks_up_screenshots(self):
        png = self.kernel.screen.screenshot_png(force=True)
        (config.SHOTS_DIR / "shot-desktop-test.png").write_bytes(png)
        self.app._refresh_shots()
        self.pump(0.2)
        self.assertIn("shot-desktop-test.png", self.app._shot_names)

    def test_audit_tab_renders_entries(self):
        self.app._refresh_audit()
        self.assertIn("kernel", self.app.audit.get("1.0", "end"))

    def test_menu_actions_open_and_close_windows(self):
        self.app.close_all()
        self.assertEqual(len(self.kernel.screen.windows), 0)
        self.app.open_app("terminal")
        self.assertTrue(any(w.app == "terminal" for w in self.kernel.screen.windows))

    # -- agent in the app --------------------------------------------------
    def test_agent_task_runs_while_the_ui_pumps(self):
        self.app.task_var.set("desktop app end to end")
        self.app.start_task()
        deadline = time.time() + 60
        while time.time() < deadline:
            self.app.root.update()
            if (not self.runner.is_running
                    and self.kernel.agent_state["state"] == "idle"
                    and self.kernel.vfs.exists("/home/user/notes/agent-report.md")):
                break
            time.sleep(0.05)
        self.app._pump_events()
        self.assertTrue(self.kernel.vfs.exists("/home/user/notes/agent-report.md"))
        text = self.app.feed.get("1.0", "end")
        self.assertIn("summary:", text)
        self.assertIn("shell_run", text)  # tool calls are visible in the app feed
        self.assertIsNotNone(self.runner.last_summary)


class TestKeyStorage(unittest.TestCase):
    def test_write_env_key_preserves_other_settings(self):
        from desktop import write_env_key

        with tempfile.TemporaryDirectory() as tmp:
            env = pathlib.Path(tmp) / ".env"
            env.write_text("SANDBOX_LLM_MODEL=meta/llama-3.3-70b-instruct\n# keep me\n", encoding="utf-8")
            write_env_key("nvapi-test-not-a-real-key", env)
            text = env.read_text(encoding="utf-8")
            self.assertIn("SANDBOX_LLM_API_KEY=nvapi-test-not-a-real-key", text)
            self.assertIn("SANDBOX_LLM_MODEL=meta/llama-3.3-70b-instruct", text)
            self.assertIn("# keep me", text)
            # rewriting replaces rather than duplicating
            write_env_key("nvapi-second-key", env)
            text = env.read_text(encoding="utf-8")
            self.assertEqual(text.count("SANDBOX_LLM_API_KEY="), 1)
            self.assertIn("nvapi-second-key", text)


if __name__ == "__main__":
    unittest.main()
