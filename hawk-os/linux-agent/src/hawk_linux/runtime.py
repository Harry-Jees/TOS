"""Read-only Docker/Podman discovery. Never exec inside a target container."""
from __future__ import annotations
import json, os, shutil, subprocess
from typing import Any

def run(args: list[str], timeout: float = 8.0) -> subprocess.CompletedProcess[str]:
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C", "LC_ALL": "C"}
    return subprocess.run(args, check=False, capture_output=True, text=True, timeout=timeout, env=env)

def discover() -> list[dict[str, Any]]:
    result = []
    for name in ("docker", "podman"):
        binary = shutil.which(name)
        if not binary:
            result.append({"runtime": name, "available": False, "error": "CLI not installed"})
            continue
        try:
            p = run([binary, "--version"])
            result.append({"runtime": name, "available": p.returncode == 0,
                           "version": p.stdout.strip() if p.returncode == 0 else None,
                           "error": p.stderr.strip()[:200] if p.returncode else None})
        except (OSError, subprocess.TimeoutExpired) as e:
            result.append({"runtime": name, "available": False, "error": str(e)[:200]})
    return result

def inventory(name: str) -> dict[str, Any]:
    binary = shutil.which(name)
    if not binary:
        return {"runtime": name, "available": False, "containers": [], "error": "CLI not installed"}
    try:
        p = run([binary, "ps", "-a", "--no-trunc", "--format", "{{json .}}"])
        if p.returncode:
            return {"runtime": name, "available": True, "containers": [], "error": p.stderr.strip()[:200]}
        containers = []
        for line in p.stdout.splitlines():
            try: item = json.loads(line)
            except json.JSONDecodeError: continue
            cid = item.get("ID") or item.get("Id")
            if not cid: continue
            q = run([binary, "inspect", str(cid)])
            if q.returncode:
                containers.append({"id": str(cid)[:64], "inspect_error": q.stderr.strip()[:200]})
                continue
            try:
                parsed = json.loads(q.stdout); cfg = parsed[0] if isinstance(parsed, list) and parsed else {}
            except (json.JSONDecodeError, IndexError, TypeError): cfg = {}
            host, conf, state = cfg.get("HostConfig") or {}, cfg.get("Config") or {}, cfg.get("State") or {}
            mounts = [{"type": str(m.get("Type", ""))[:40],
                       "destination": str(m.get("Destination", ""))[:250],
                       "rw": bool(m.get("RW", False)),
                       "source_redacted": bool(m.get("Source"))} for m in cfg.get("Mounts") or []]
            env_names = sorted({e.split("=", 1)[0][:100] for e in conf.get("Env") or [] if isinstance(e, str) and "=" in e})
            containers.append({
                "id": str(cid)[:64], "name": str(cfg.get("Name") or item.get("Names") or "")[:200],
                "image": str(conf.get("Image") or item.get("Image") or "")[:200],
                "state": str(state.get("Status") or item.get("State") or "")[:80],
                "user": str(conf.get("User") or "")[:100],
                "privileged": bool(host.get("Privileged", False)),
                "pid_mode": str(host.get("PidMode") or ""), "network_mode": str(host.get("NetworkMode") or ""),
                "ipc_mode": str(host.get("IpcMode") or ""), "readonly_rootfs": bool(host.get("ReadonlyRootfs", False)),
                "cap_add": [str(x)[:80] for x in (host.get("CapAdd") or [])],
                "security_opt": [str(x)[:120] for x in (host.get("SecurityOpt") or [])],
                "mounts": mounts, "environment_variable_names": env_names[:200]})
        return {"runtime": name, "available": True, "containers": containers}
    except (OSError, subprocess.TimeoutExpired) as e:
        return {"runtime": name, "available": True, "containers": [], "error": str(e)[:200]}
