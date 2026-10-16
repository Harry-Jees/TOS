"""The jail must be a jail: no path may reach the host filesystem."""

from __future__ import annotations

import os
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from sandboxos.vfs import QuotaExceeded, Vfs, VfsError, normalize  # noqa: E402


class TestNormalize(unittest.TestCase):
    def test_accepts_sandbox_paths(self):
        self.assertEqual(normalize("/home/user/a.txt"), "/home/user/a.txt")
        self.assertEqual(normalize("home/user/../user/x"), "/home/user/x")
        self.assertEqual(normalize(""), "/")
        self.assertEqual(normalize("/"), "/")

    def test_rejects_escapes(self):
        for bad in (
            "/../../etc/passwd",
            "/..",
            "../../secret",
            "..\\..\\windows\\system32",
            "C:\\Windows\\System32",
            "\\\\server\\share",
            "/a/../../../b",
            "/home/\x00user",
        ):
            with self.subTest(path=bad):
                with self.assertRaises(VfsError):
                    normalize(bad)

    def test_rejects_reserved_names(self):
        with self.assertRaises(VfsError):
            normalize("/tmp/nul")


class TestVfs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name) / "disk"
        self.vfs = Vfs(self.root, quota_bytes=4096, max_file_bytes=1024, max_entries=50)

    def tearDown(self):
        self.tmp.cleanup()

    def test_write_read_roundtrip(self):
        self.vfs.mkdir("/home/user")
        res = self.vfs.write_text("/home/user/a.txt", "hello")
        self.assertEqual(res["bytes_written"], 5)
        self.assertEqual(self.vfs.read_text("/home/user/a.txt"), "hello")
        self.assertTrue(self.vfs.is_file("/home/user/a.txt"))

    def test_append(self):
        self.vfs.write_text("/f.txt", "a")
        self.vfs.write_text("/f.txt", "b", append=True)
        self.assertEqual(self.vfs.read_text("/f.txt"), "ab")

    def test_missing_file(self):
        with self.assertRaises(VfsError):
            self.vfs.read_text("/nope.txt")

    def test_escape_is_blocked(self):
        with self.assertRaises(VfsError):
            self.vfs.write_text("/../../pwned.txt", "x")
        # and nothing was created next to the sandbox disk
        self.assertFalse((self.root.parent / "pwned.txt").exists())

    def test_quota(self):
        with self.assertRaises(QuotaExceeded):
            self.vfs.write_text("/big.bin", "x" * 2000)
        self.vfs.write_text("/small.txt", "x" * 100)

    def test_per_file_limit(self):
        with self.assertRaises(QuotaExceeded):
            self.vfs.write_text("/max.txt", "x" * 1500)

    def test_list_and_stat(self):
        self.vfs.mkdir("/d")
        self.vfs.write_text("/d/a.txt", "12")
        entries = self.vfs.list_dir("/d")
        self.assertEqual([e["name"] for e in entries], ["a.txt"])
        st = self.vfs.stat("/d/a.txt")
        self.assertEqual(st["type"], "file")
        self.assertEqual(st["size"], 2)

    def test_remove_recursive(self):
        self.vfs.mkdir("/d/sub")
        self.vfs.write_text("/d/sub/a.txt", "x")
        with self.assertRaises(VfsError):
            self.vfs.remove("/d")
        res = self.vfs.remove("/d", recursive=True)
        self.assertGreaterEqual(res["removed"], 2)
        self.assertFalse(self.vfs.exists("/d"))

    def test_move_and_copy(self):
        self.vfs.write_text("/a.txt", "abc")
        self.vfs.move("/a.txt", "/b.txt")
        self.assertFalse(self.vfs.exists("/a.txt"))
        self.vfs.copy("/b.txt", "/c.txt")
        self.assertEqual(self.vfs.read_text("/c.txt"), "abc")

    def test_search(self):
        self.vfs.mkdir("/src")
        self.vfs.write_text("/src/a.py", "hello world\nsecond line\n")
        self.vfs.write_text("/src/b.py", "nothing here\n")
        hits = self.vfs.search("world", "/")
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0]["path"], "/src/a.py")
        self.assertEqual(hits[0]["line"], 1)

    def test_usage_counts(self):
        self.vfs.write_text("/a.txt", "12345")
        usage = self.vfs.usage()
        self.assertEqual(usage["files"], 1)
        self.assertEqual(usage["used_bytes"], 5)

    def test_symlink_escape(self):
        """A symlink inside the sandbox pointing outside must not be readable."""
        outside = pathlib.Path(self.tmp.name) / "outside.txt"
        outside.write_text("host secret")
        link = self.root / "link.txt"
        try:
            os.symlink(outside, link)
        except (OSError, NotImplementedError):
            self.skipTest("symlinks not available on this platform")
        with self.assertRaises(VfsError):
            self.vfs.read_text("/link.txt")


if __name__ == "__main__":
    unittest.main()
