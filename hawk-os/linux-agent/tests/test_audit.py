import unittest
from hawk_linux.audit import audit

class AuditTests(unittest.TestCase):
    def test_flags_privileged_and_runtime_socket(self):
        findings = audit({"runtime": "docker", "containers": [{
            "name": "test", "privileged": True, "mounts": [
                {"destination": "/var/run/docker.sock", "rw": True}],
            "readonly_rootfs": False, "user": ""}]})
        checks = {x["check"] for x in findings}
        self.assertIn("privileged_container", checks)
        self.assertIn("runtime_socket_mount", checks)
        self.assertIn("writable_rootfs", checks)
        self.assertIn("unspecified_user", checks)

    def test_no_findings_for_hardened_container(self):
        findings = audit({"runtime": "podman", "containers": [{
            "name": "safe", "privileged": False, "pid_mode": "private",
            "network_mode": "bridge", "ipc_mode": "private", "readonly_rootfs": True,
            "user": "1000:1000", "cap_add": [], "security_opt": [],
            "mounts": []}]})
        self.assertEqual(findings, [])

if __name__ == "__main__": unittest.main()
