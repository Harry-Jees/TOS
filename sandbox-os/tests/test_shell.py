"""The shell must be useful and completely unable to reach the host."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from sandboxos.kernel import Kernel  # noqa: E402
from sandboxos.shell import ShellSession, run_line  # noqa: E402


class ShellCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls._tmp.name)
        cls.kernel = Kernel(root=base / "disk", var_dir=base / "var", boot_windows=True)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def setUp(self):
        self.session = ShellSession(self.kernel)
        # reset the user's home each test for isolation
        self.kernel.vfs.remove("/home/user", recursive=True)
        self.kernel.vfs.mkdir("/home/user/notes")
        self.kernel.vfs.write_text("/home/user/notes/todo.md", "alpha\nbeta\n")

    def run_cmd(self, line: str):
        return run_line(self.session, line)


class TestShellBasics(ShellCase):
    def test_pwd_ls_cd(self):
        self.assertEqual(self.run_cmd("pwd").out, "/home/user")
        out = self.run_cmd("ls").out
        self.assertIn("notes", out)
        self.run_cmd("cd /etc")
        self.assertEqual(self.run_cmd("pwd").out, "/etc")
        self.run_cmd("cd -")
        self.assertEqual(self.run_cmd("pwd").out, "/home/user")

    def test_redirect_write_and_append(self):
        self.run_cmd("echo hello > out.txt")
        self.assertEqual(self.kernel.vfs.read_text("/home/user/out.txt").strip(), "hello")
        self.run_cmd("echo world >> out.txt")
        self.assertEqual(self.kernel.vfs.read_text("/home/user/out.txt").split(), ["hello", "world"])

    def test_pipes(self):
        out = self.run_cmd("echo hello | wc -c").out
        self.assertEqual(out.split()[2], "6")  # "hello\n"
        out = self.run_cmd("seq 1 6 | grep 4").out
        self.assertEqual(out, "4")

    def test_and_or(self):
        self.assertEqual(self.run_cmd("true_cmd ; echo second").out.strip(), "second")
        res = self.run_cmd("cat /nope.txt && echo unreachable")
        self.assertEqual(res.code, 1)
        self.assertNotIn("unreachable", res.out)
        res = self.run_cmd("cat /nope.txt || echo fallback")
        self.assertIn("fallback", res.out)

    def test_grep_recursive(self):
        out = self.run_cmd("grep -r -n beta /home/user").out
        self.assertIn("todo.md", out)
        self.assertIn("beta", out)

    def test_head_tail_wc(self):
        self.assertEqual(self.run_cmd("head -n 1 /home/user/notes/todo.md").out.strip(), "alpha")
        self.assertEqual(self.run_cmd("tail -n 1 /home/user/notes/todo.md").out, "beta")

    def test_glob(self):
        self.kernel.vfs.write_text("/home/user/a.txt", "x")
        self.kernel.vfs.write_text("/home/user/b.txt", "y")
        out = self.run_cmd("ls *.txt").out
        self.assertIn("a.txt", out)
        self.assertIn("b.txt", out)

    def test_tree(self):
        self.assertIn("todo.md", self.run_cmd("tree /home/user").out)

    def test_variables(self):
        self.run_cmd("export GREETING=hi")
        self.assertEqual(self.run_cmd("echo $GREETING").out.strip(), "hi")

    def test_history(self):
        self.run_cmd("echo one")
        self.run_cmd("echo two")
        out = self.run_cmd("history").out
        self.assertIn("echo one", out)
        self.assertIn("echo two", out)

    def test_df_and_usage(self):
        self.assertIn("sandbox", self.run_cmd("df").out)

    def test_neofetch_mentions_simulation(self):
        self.assertIn("simulated", self.run_cmd("neofetch").out)


class TestShellSandbox(unittest.TestCase):
    """There must be no host escape route at all."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls._tmp.name)
        cls.kernel = Kernel(root=base / "disk", var_dir=base / "var", boot_windows=False)
        cls.session = ShellSession(cls.kernel)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_unix_binaries_do_not_exist(self):
        for cmd in ("python -c 'print(1)'", "bash -c 'id'", "curl http://example.com",
                    "powershell -c dir", "cmd /c dir", "nc -l 1234"):
            with self.subTest(cmd=cmd):
                res = run_line(self.session, cmd)
                self.assertEqual(res.code, 127, res.text)

    def test_host_etc_and_drive_are_unreachable(self):
        for cmd in ("cat /etc/passwd", "ls C:\\Windows", "cat ../../../../etc/passwd",
                    "cat /../secret.txt"):
            with self.subTest(cmd=cmd):
                res = run_line(self.session, cmd)
                self.assertNotEqual(res.code, 0)
                self.assertNotIn("bin", res.out.lower())

    def test_cd_cannot_climb_out_of_the_sandbox(self):
        before = self.session.cwd
        res = run_line(self.session, "cd /..")
        self.assertNotEqual(res.code, 0)
        self.assertIn("escape", res.text.lower())
        self.assertEqual(self.session.cwd, before)
        # climbing above the root with relative parts is refused too
        res = run_line(self.session, "cd ../../../..")
        self.assertNotEqual(res.code, 0)
        self.assertEqual(self.session.cwd, before)
        # but normal ".." inside the tree still works
        self.assertEqual(run_line(self.session, "cd ..").code, 0)
        self.assertEqual(self.session.cwd, "/home")

    def test_sudo_explains_itself(self):
        out = run_line(self.session, "sudo rm -rf /").text
        self.assertIn("no host to escalate to", out)

    def test_deleting_root_is_refused(self):
        res = run_line(self.session, "rm -r /")
        self.assertNotEqual(res.code, 0)
        self.assertTrue(self.kernel.vfs.exists("/etc/motd"))

    def test_output_is_truncated(self):
        res = run_line(self.session, "seq 1 9000")
        self.assertIn("truncated", res.out)


if __name__ == "__main__":
    unittest.main()
