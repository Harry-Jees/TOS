"""Heuristic, non-invasive container configuration checks."""
from typing import Any

def audit(data: dict[str, Any]) -> list[dict[str, str]]:
    findings = []
    def add(c: dict[str, Any], check: str, severity: str, detail: str) -> None:
        findings.append({"runtime": str(data.get("runtime", "unknown")),
          "container": str(c.get("name") or c.get("id") or "unknown"),
          "check": check, "severity": severity, "detail": detail})
    for c in data.get("containers", []):
        if c.get("privileged"): add(c, "privileged_container", "high", "Privileged mode substantially weakens isolation.")
        if c.get("pid_mode") == "host": add(c, "host_pid_namespace", "high", "Shares host PID namespace.")
        if c.get("network_mode") == "host": add(c, "host_network_namespace", "medium", "Shares host network namespace.")
        if c.get("ipc_mode") == "host": add(c, "host_ipc_namespace", "medium", "Shares host IPC namespace.")
        for m in c.get("mounts", []):
            dest = (m.get("destination") or "").rstrip("/") or "/"
            if dest in ("/var/run/docker.sock", "/run/podman/podman.sock"):
                add(c, "runtime_socket_mount", "critical", "Container can access a container-runtime socket.")
            elif m.get("rw") and dest in ("/", "/etc", "/proc", "/sys", "/dev"):
                add(c, "sensitive_writable_mount", "high", f"Writable mount targets sensitive path {dest}.")
        caps = {str(x).upper() for x in c.get("cap_add", [])}
        for cap in sorted(caps & {"SYS_ADMIN", "SYS_PTRACE", "DAC_READ_SEARCH", "NET_ADMIN"}):
            add(c, "extra_capability", "high" if cap in {"SYS_ADMIN", "SYS_PTRACE"} else "medium",
                f"Additional capability granted: {cap}.")
        opts = " ".join(str(x).lower() for x in c.get("security_opt", []))
        if "seccomp=unconfined" in opts: add(c, "seccomp_unconfined", "high", "Seccomp is unconfined.")
        if "apparmor=unconfined" in opts: add(c, "apparmor_unconfined", "high", "AppArmor is unconfined.")
        if not c.get("readonly_rootfs", False): add(c, "writable_rootfs", "low", "Container root filesystem is writable.")
        if not str(c.get("user") or "").strip(): add(c, "unspecified_user", "medium", "No explicit container user; runtime may default to root.")
    return findings
