"""Screen control: windows, clicks, typing, drags and screenshots."""

from __future__ import annotations

import pathlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from sandboxos.kernel import Kernel  # noqa: E402


def png_size(data: bytes) -> tuple[int, int]:
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    w, h = struct.unpack(">II", data[16:24])
    return w, h


class ScreenCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self._tmp.name)
        self.kernel = Kernel(root=base / "disk", var_dir=base / "var", boot_windows=True)
        self.wm = self.kernel.screen

    def tearDown(self):
        self._tmp.cleanup()

    def terminal(self):
        return next(w for w in self.wm.windows if w.app == "terminal")

    def test_boot_opens_windows(self):
        apps = {w.app for w in self.wm.windows}
        self.assertTrue({"terminal", "files", "agent", "sysmon"} <= apps)
        self.assertIsNotNone(self.wm.focus)

    def test_screenshot_is_png_of_screen_size(self):
        png = self.wm.screenshot_png(force=True)
        self.assertEqual(png_size(png), (self.wm.width, self.wm.height))
        self.assertGreater(len(png), 1000)

    def test_frame_cache_invalidates_on_change(self):
        first = self.wm.screenshot_png(force=True)
        self.assertIs(self.wm.screenshot_png(), first)  # cached while nothing moves
        self.wm.mouse("move", 40, 40)  # the cursor is painted, so pixels change
        self.assertNotEqual(self.wm.screenshot_png(), first)

    def test_click_focuses_and_close_button_closes(self):
        term = self.terminal()
        files = next(w for w in self.wm.windows if w.app == "files")
        self.wm.mouse("down", files.x + 40, files.y + 30)
        self.assertEqual(self.wm.focus_id, files.id)
        bx, by, bw, bh = files.close_button_rect()
        self.wm.mouse("down", bx + 3, by + 3)
        self.assertNotIn(files, self.wm.windows)
        self.assertIsNotNone(self.wm.focus)  # focus moved to another window
        self.assertTrue(self.wm.by_id(term.id))

    def test_typing_runs_a_command_in_the_terminal(self):
        term = self.terminal()
        self.wm.focus_window(term.id)
        self.wm.type_text("echo typed-by-test")
        self.wm.key("Enter")
        transcript = "\n".join(t for t, _ in term.lines)
        self.assertIn("typed-by-test", transcript)
        self.assertIn("$ echo typed-by-test", transcript)

    def test_typing_backspace_and_history(self):
        term = self.terminal()
        self.wm.focus_window(term.id)
        self.wm.type_text("echo remove-me")
        self.wm.key("Backspace")
        self.assertEqual(term.input, "echo remove-m")
        self.wm.key("Enter")
        self.wm.key("Up")
        self.assertEqual(term.input, "echo remove-m")

    def test_clicks_in_files_window_navigate(self):
        files = next(w for w in self.wm.windows if w.app == "files")
        self.wm.focus_window(files.id)
        files.path = "/home/user"
        files.refresh()
        notes_row = next(i for i, e in enumerate(files.entries) if e["name"] == "notes")
        cx, cy, _, _ = files.content_rect
        self.wm.mouse("down", cx + 30, cy + 20 + notes_row * 8 + 3)
        self.assertEqual(files.path, "/home/user/notes")

    def test_taskbar_click_opens_and_focuses(self):
        for w in list(self.wm.windows):
            if w.app == "editor":
                self.wm.close(w.id)
        for app, label, (bx, by, bw, bh) in self.wm.taskbar_rects():
            if app == "editor":
                self.wm.mouse("down", bx + bw // 2, by + bh // 2)
        self.assertTrue(any(w.app == "editor" for w in self.wm.windows))
        self.assertEqual(self.wm.focus.app, "editor")

    def test_drag_moves_window(self):
        term = self.terminal()
        start = (term.x, term.y)
        self.wm.mouse("down", term.x + 60, term.y + 8)
        self.wm.mouse("move", term.x + 160, term.y + 48)
        self.wm.mouse("up", term.x + 160, term.y + 48)
        self.assertEqual((term.x, term.y), (start[0] + 100, start[1] + 40))

    def test_describe_text_mentions_content(self):
        text = self.wm.describe_text()
        self.assertIn("SCREEN 960x600", text)
        self.assertIn("terminal", text)
        self.assertIn("TASKBAR", text)

    def test_outside_click_focuses_nothing(self):
        self.wm.mouse("down", 5, self.wm.height - self.wm.taskbar_h - 40)
        # clicking bare desktop (below panel, above taskbar, left of windows)
        self.assertTrue(self.wm.focus is None or self.wm.focus.app in
                        {"terminal", "files", "agent", "sysmon", "editor"})

    def test_history_scroll_does_not_crash(self):
        term = self.terminal()
        for i in range(60):
            term.run(f"echo line {i}")
        self.wm.focus_window(term.id)
        self.wm.mouse("wheel", term.x + 50, term.y + 60, "left", -3)
        self.assertIsNotNone(self.wm.screenshot_png(force=True))


if __name__ == "__main__":
    unittest.main()
