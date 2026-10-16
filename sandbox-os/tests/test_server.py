"""The console server: live screen, state, agent tasks and human input."""

from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import time
import unittest
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from agentd import AgentRunner, ScriptedProvider  # noqa: E402
from sandboxos import config  # noqa: E402
from sandboxos.kernel import Kernel  # noqa: E402
from server import ControlServer  # noqa: E402


def get(url: str):
    with urllib.request.urlopen(url, timeout=10) as resp:
        return resp.status, resp.read(), resp.headers.get("Content-Type", "")


def post(url: str, payload: dict):
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"}, method="POST"
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.status, json.loads(resp.read().decode())


class TestConsoleServer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls._tmp.name)
        cls._old_shots = config.SHOTS_DIR
        config.SHOTS_DIR = base / "shots"
        config.SHOTS_DIR.mkdir()
        cls.kernel = Kernel(root=base / "disk", var_dir=base / "var", boot_windows=True)
        cls.runner = AgentRunner(cls.kernel, provider=ScriptedProvider())
        cls.server = ControlServer(cls.kernel, cls.runner, port=8899)
        cls.port = cls.server.start()
        cls.base = f"http://127.0.0.1:{cls.port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()
        config.SHOTS_DIR = cls._old_shots
        cls._tmp.cleanup()

    def test_index_and_assets(self):
        status, body, ctype = get(self.base + "/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", ctype)
        self.assertIn(b"SandboxOS", body)
        status, body, ctype = get(self.base + "/app.js")
        self.assertEqual(status, 200)
        self.assertIn("javascript", ctype)

    def test_screen_png(self):
        status, body, ctype = get(self.base + "/screen.png")
        self.assertEqual(status, 200)
        self.assertEqual(ctype, "image/png")
        self.assertTrue(body.startswith(b"\x89PNG"))
        self.assertGreater(len(body), 1000)

    def test_state_reports_machine_and_events(self):
        status, body, _ = get(self.base + "/state")
        data = json.loads(body)
        self.assertTrue(data["ok"])
        self.assertEqual(data["screen"]["w"], 960)
        self.assertGreaterEqual(data["sysinfo"]["windows"], 1)
        self.assertIn("disk", data["sysinfo"])
        self.assertTrue(data["provider_name"])

    def test_shell_endpoint(self):
        status, data = post(self.base + "/shell", {"command": "echo from-http"})
        self.assertTrue(data["ok"])
        self.assertIn("from-http", data["output"])

    def test_human_input_endpoint(self):
        status, data = post(self.base + "/input", {"type": "mouse", "action": "move", "x": 100, "y": 90})
        self.assertTrue(data["ok"])
        self.assertTrue(data["result"]["handled"])
        status, data = post(self.base + "/input", {"type": "key", "key": "a", "text": "a"})
        self.assertTrue(data["ok"])

    def test_open_endpoint(self):
        status, data = post(self.base + "/open", {"app": "editor", "path": "/etc/motd"})
        self.assertTrue(data["ok"])
        self.assertTrue(any(w.app == "editor" for w in self.kernel.screen.windows))

    def test_prompt_runs_the_agent_and_serves_screenshots(self):
        status, data = post(self.base + "/prompt", {"task": "run the http integration demo"})
        self.assertTrue(data["ok"])
        deadline = time.time() + 45
        while time.time() < deadline:
            if self.kernel.vfs.exists("/home/user/notes/agent-report.md") and not self.runner.is_running:
                break
            time.sleep(0.3)
        self.assertTrue(self.kernel.vfs.exists("/home/user/notes/agent-report.md"),
                        "agent did not finish its scripted task")
        # screenshots are listed and downloadable
        _, body, _ = get(self.base + "/state")
        shots = json.loads(body)["shots"]
        self.assertTrue(shots, "no screenshots recorded")
        status, img, ctype = get(self.base + urllib.parse.quote(shots[0]["url"]))
        self.assertEqual(status, 200)
        self.assertTrue(img.startswith(b"\x89PNG"))

    def test_state_stays_responsive_while_agent_works(self):
        post(self.base + "/prompt", {"task": "long-ish task"})
        t0 = time.time()
        status, body, _ = get(self.base + "/state")
        self.assertEqual(status, 200)
        self.assertLess(time.time() - t0, 5)

    def test_abort_endpoint(self):
        status, data = post(self.base + "/abort", {})
        self.assertTrue(data["ok"])

    def test_unknown_route_404(self):
        try:
            get(self.base + "/nope")
            self.fail("expected 404")
        except urllib.error.HTTPError as exc:
            self.assertEqual(exc.code, 404)

    def test_audit_endpoint(self):
        status, body, _ = get(self.base + "/audit?n=5")
        data = json.loads(body)
        self.assertTrue(data["ok"])
        self.assertIsInstance(data["entries"], list)


import urllib.error  # noqa: E402  (used by test_unknown_route_404)
import urllib.parse  # noqa: E402  (used by the screenshot test)

if __name__ == "__main__":
    unittest.main()
