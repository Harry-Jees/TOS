"""The SandboxOS shell.

A real interpreter, not a subprocess. It supports quoting, globs, pipes,
redirection, sequences (; && ||) and a small POSIX-ish command set - all
implemented against the jailed VFS. There is deliberately no `exec`, no host
path access and no way to reach the host operating system.
"""

from __future__ import annotations

import fnmatch
import posixpath
import shlex
import time

from .vfs import VfsError, normalize

VERSION = "SandboxOS 1.0 (kernel 6.0-sandbox)"
MAX_OUTPUT_CHARS = 20_000


class ShellSession:
    """One interactive session: working directory, variables, history."""

    def __init__(self, kernel, user: str = "user", host: str = "sandbox"):
        self.kernel = kernel
        self.user = user
        self.host = host
        self.cwd = f"/home/{user}"
        self.env = {
            "USER": user,
            "HOME": f"/home/{user}",
            "HOSTNAME": host,
            "SHELL": "/bin/sssh",
            "PATH": "/bin:/usr/bin",
            "TERM": "sandbox-256",
        }
        self.history: list[str] = []

    # -- path helpers ------------------------------------------------------
    def abspath(self, path: str) -> str:
        if not path:
            return self.cwd
        if path.startswith("/"):
            return normalize(path)
        return normalize(posixpath.join(self.cwd, path))

    def prompt(self) -> str:
        return f"{self.user}@{self.host}:{self.cwd}$ "


def _expand(session: ShellSession, token: str) -> list[str]:
    """Expand a path token, including simple globs."""
    if any(ch in token for ch in "*?[") and not token.startswith("-"):
        base = session.abspath(posixpath.dirname(token) or ".")
        pattern = posixpath.basename(token)
        matches = session.kernel.vfs.find(base, pattern, dirs=True)
        names = [
            posixpath.join(base, posixpath.basename(m["path"]))
            for m in matches
        ]
        return names or [token]
    if "$" in token:
        return [_expand_vars(session, token)]
    return [token]


def _expand_vars(session: ShellSession, token: str) -> str:
    out = []
    i = 0
    while i < len(token):
        ch = token[i]
        if ch == "$":
            j = i + 1
            if j < len(token) and token[j] == "{":
                k = token.find("}", j)
                name = token[j + 1 : k] if k > -1 else token[j + 1 :]
                i = k + 1 if k > -1 else len(token)
            else:
                k = j
                while k < len(token) and (token[k].isalnum() or token[k] == "_"):
                    k += 1
                name = token[j:k]
                i = k
            out.append(session.env.get(name, ""))
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def tokenize(line: str) -> list[str]:
    if not line.strip():
        return []
    try:
        return shlex.split(line, posix=True)
    except ValueError as exc:
        raise ShellSyntaxError(str(exc)) from exc


class ShellSyntaxError(Exception):
    pass


class CommandResult:
    def __init__(self, out: str = "", code: int = 0, stderr: str = ""):
        self.out = out
        self.code = code
        self.stderr = stderr

    @property
    def text(self) -> str:
        parts = [p for p in (self.out, self.stderr) if p]
        return "\n".join(parts)


# --------------------------------------------------------------------------
# command implementations: fn(session, args, stdin) -> CommandResult
# --------------------------------------------------------------------------
def _c_help(s, args, stdin):
    names = " ".join(sorted(COMMANDS))
    return CommandResult(
        "SandboxOS built-in commands (no host binaries exist here):\n"
        f"  {names}\n\n"
        "Supported shell syntax:  cmd -flag arg  |  ;  &&  ||  >  >>  'quote'  "
        '"quote"  *.glob  $VAR\n'
        "There is no host escape: the sandbox has its own filesystem and no "
        "network."
    )


def _c_pwd(s, args, stdin):
    return CommandResult(s.cwd)


def _c_cd(s, args, stdin):
    target = args[0] if args else s.env.get("HOME", "/")
    if target == "-":
        target = s.env.get("OLDPWD", s.cwd)
    if s.cwd == "/" and target.replace("\\", "/").strip("/") in ("..", "../.."):
        return CommandResult("")  # already at the root of the sandbox
    path = s.abspath(target)
    if not s.kernel.vfs.exists(path):
        return CommandResult(code=1, stderr=f"cd: no such directory: {target}")
    if not s.kernel.vfs.is_dir(path):
        return CommandResult(code=1, stderr=f"cd: not a directory: {target}")
    s.env["OLDPWD"] = s.cwd
    s.cwd = path
    return CommandResult("")


def _c_ls(s, args, stdin):
    flags = {a for a in args if a.startswith("-")}
    paths = [a for a in args if not a.startswith("-")] or [s.cwd]
    long_fmt = any("l" in f for f in flags)
    all_fmt = any("a" in f for f in flags)
    lines: list[str] = []
    for target in paths:
        path = s.abspath(target)
        try:
            if s.kernel.vfs.is_file(path):
                st = s.kernel.vfs.stat(path)
                lines.append(f"{st['size']:>8}  {posixpath.basename(path)}")
                continue
            entries = s.kernel.vfs.list_dir(path)
        except VfsError as exc:
            return CommandResult(code=1, stderr=f"ls: {exc}")
        if not all_fmt:
            entries = [e for e in entries if not e["name"].startswith(".")]
        if len(paths) > 1:
            lines.append(f"{target}:")
        for e in entries:
            if long_fmt:
                kind = "d" if e["type"] == "dir" else "-"
                when = time.strftime("%b %d %H:%M", time.localtime(e["mtime"]))
                lines.append(
                    f"{kind}rw-r--r--  1 {s.user} {s.user} "
                    f"{e['size']:>8} {when} {e['name']}"
                    + ("/" if e["type"] == "dir" else "")
                )
            else:
                lines.append(e["name"] + ("/" if e["type"] == "dir" else ""))
    return CommandResult("\n".join(lines))


def _c_cat(s, args, stdin):
    if not args:
        return CommandResult(stdin)
    out = []
    for a in args:
        path = s.abspath(a)
        try:
            if s.kernel.vfs.is_dir(path):
                return CommandResult(code=1, stderr=f"cat: {a}: is a directory")
            out.append(s.kernel.vfs.read_text(path))
        except VfsError as exc:
            return CommandResult(code=1, stderr=f"cat: {exc}")
    return CommandResult("\n".join(out))


def _c_echo(s, args, stdin):
    return CommandResult(" ".join(args) + "\n")


def _c_mkdir(s, args, stdin):
    paths = [a for a in args if not a.startswith("-")]
    if not paths:
        return CommandResult(code=1, stderr="mkdir: missing operand")
    for a in paths:
        try:
            s.kernel.vfs.mkdir(s.abspath(a))
        except VfsError as exc:
            return CommandResult(code=1, stderr=f"mkdir: {exc}")
    return CommandResult("")


def _c_touch(s, args, stdin):
    if not args:
        return CommandResult(code=1, stderr="touch: missing operand")
    for a in args:
        path = s.abspath(a)
        try:
            if not s.kernel.vfs.exists(path):
                s.kernel.vfs.write_text(path, "")
        except VfsError as exc:
            return CommandResult(code=1, stderr=f"touch: {exc}")
    return CommandResult("")


def _c_rm(s, args, stdin):
    rc = any("r" in f for f in args if f.startswith("-"))
    paths = [a for a in args if not a.startswith("-")]
    if not paths:
        return CommandResult(code=1, stderr="rm: missing operand")
    for a in paths:
        try:
            s.kernel.vfs.remove(s.abspath(a), recursive=rc)
        except VfsError as exc:
            return CommandResult(code=1, stderr=f"rm: {exc}")
    return CommandResult("")


def _c_cp(s, args, stdin):
    paths = [a for a in args if not a.startswith("-")]
    if len(paths) != 2:
        return CommandResult(code=1, stderr="cp: usage: cp SRC DST")
    try:
        s.kernel.vfs.copy(s.abspath(paths[0]), s.abspath(paths[1]))
    except VfsError as exc:
        return CommandResult(code=1, stderr=f"cp: {exc}")
    return CommandResult("")


def _c_mv(s, args, stdin):
    paths = [a for a in args if not a.startswith("-")]
    if len(paths) != 2:
        return CommandResult(code=1, stderr="mv: usage: mv SRC DST")
    try:
        s.kernel.vfs.move(s.abspath(paths[0]), s.abspath(paths[1]))
    except VfsError as exc:
        return CommandResult(code=1, stderr=f"mv: {exc}")
    return CommandResult("")


def _c_head(s, args, stdin):
    n = 10
    if "-n" in args:
        i = args.index("-n")
        try:
            n = int(args[i + 1])
        except (IndexError, ValueError):
            return CommandResult(code=1, stderr="head: invalid number")
    files = [a for a in args if not a.startswith("-") and not a.isdigit()]
    if not files:
        return CommandResult("\n".join(stdin.splitlines()[:n]))
    out = []
    for f in files:
        try:
            out.append("\n".join(s.kernel.vfs.read_text(s.abspath(f)).splitlines()[:n]))
        except VfsError as exc:
            return CommandResult(code=1, stderr=f"head: {exc}")
    return CommandResult("\n".join(out))


def _c_tail(s, args, stdin):
    n = 10
    if "-n" in args:
        i = args.index("-n")
        try:
            n = int(args[i + 1])
        except (IndexError, ValueError):
            return CommandResult(code=1, stderr="tail: invalid number")
    files = [a for a in args if not a.startswith("-") and not a.isdigit()]
    if not files:
        return CommandResult("\n".join(stdin.splitlines()[-n:]))
    out = []
    for f in files:
        try:
            out.append("\n".join(s.kernel.vfs.read_text(s.abspath(f)).splitlines()[-n:]))
        except VfsError as exc:
            return CommandResult(code=1, stderr=f"tail: {exc}")
    return CommandResult("\n".join(out))


def _c_wc(s, args, stdin):
    files = [a for a in args if not a.startswith("-")]
    texts = []
    for f in files:
        try:
            texts.append((f, s.kernel.vfs.read_text(s.abspath(f))))
        except VfsError as exc:
            return CommandResult(code=1, stderr=f"wc: {exc}")
    if not texts:
        texts = [("", stdin)]
    lines_out = []
    for name, text in texts:
        l = len(text.splitlines())
        w = len(text.split())
        c = len(text)
        lines_out.append(f"{l:>7} {w:>7} {c:>7} {name}".rstrip())
    return CommandResult("\n".join(lines_out))


def _c_grep(s, args, stdin):
    ignore = any("i" in f for f in args if f.startswith("-"))
    numbers = any("n" in f for f in args if f.startswith("-"))
    recurse = any("r" in f for f in args if f.startswith("-"))
    rest = [a for a in args if not a.startswith("-")]
    if not rest:
        return CommandResult(code=1, stderr="grep: usage: grep [-i -n -r] PATTERN [FILE...]")
    pattern, files = rest[0], rest[1:]
    try:
        if recurse:
            target = s.abspath(files[0]) if files else s.cwd
            hits = s.kernel.vfs.search(pattern, target, ignore_case=ignore)
            return CommandResult(
                "\n".join(
                    (f"{h['path']}:{h['line']}:{h['text']}" if numbers or recurse else h["text"])
                    for h in hits
                )
            )
        out: list[str] = []
        if files:
            for f in files:
                text = s.kernel.vfs.read_text(s.abspath(f))
                for i, line in enumerate(text.splitlines(), 1):
                    hit = pattern.lower() in line.lower() if ignore else pattern in line
                    if hit:
                        out.append(f"{i}:{line}" if numbers else line)
        else:
            # no files: filter the piped input, exactly like grep on a pipe
            for i, line in enumerate(stdin.splitlines(), 1):
                hit = pattern.lower() in line.lower() if ignore else pattern in line
                if hit:
                    out.append(f"{i}:{line}" if numbers else line)
        return CommandResult("\n".join(out))
    except VfsError as exc:
        return CommandResult(code=1, stderr=f"grep: {exc}")


def _c_find(s, args, stdin):
    start = args[0] if args and not args[0].startswith("-") else s.cwd
    name = "*"
    if "-name" in args:
        i = args.index("-name")
        if i + 1 < len(args):
            name = args[i + 1]
    try:
        hits = s.kernel.vfs.find(s.abspath(start), name, dirs=True)
    except VfsError as exc:
        return CommandResult(code=1, stderr=f"find: {exc}")
    return CommandResult("\n".join(h["path"] for h in hits))


def _c_tree(s, args, stdin):
    depth = 3
    if "-L" in args:
        i = args.index("-L")
        if i + 1 < len(args):
            try:
                depth = int(args[i + 1])
            except ValueError:
                depth = 3
    target = next((a for a in args if not a.startswith("-") and not a.isdigit()), s.cwd)
    try:
        return CommandResult(s.kernel.vfs.tree(s.abspath(target), depth))
    except VfsError as exc:
        return CommandResult(code=1, stderr=f"tree: {exc}")


def _c_stat_cmd(s, args, stdin):
    if not args:
        return CommandResult(code=1, stderr="stat: missing operand")
    out = []
    for a in args:
        try:
            st = s.kernel.vfs.stat(s.abspath(a))
        except VfsError as exc:
            return CommandResult(code=1, stderr=f"stat: {exc}")
        when = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(st["mtime"]))
        out.append(f"  File: {st['path']}\n  Type: {st['type']}\n  Size: {st['size']}\n  Modified: {when}")
    return CommandResult("\n".join(out))


def _c_date(s, args, stdin):
    return CommandResult(time.strftime("%a %b %d %H:%M:%S %Y (sandbox clock)"))


def _c_whoami(s, args, stdin):
    return CommandResult(s.user)


def _c_id(s, args, stdin):
    return CommandResult(f"uid=1000({s.user}) gid=1000({s.user}) groups=1000({s.user}),0(root)")


def _c_hostname(s, args, stdin):
    return CommandResult(s.host)


def _c_uname(s, args, stdin):
    if "-a" in args or not args:
        return CommandResult(f"SandboxOS {s.host} 6.0-sandbox #1 SMP virtual x86_64 SandboxOS")
    return CommandResult("SandboxOS")


def _c_df(s, args, stdin):
    u = s.kernel.vfs.usage()
    pct = int(100 * u["used_bytes"] / max(1, u["quota_bytes"]))
    return CommandResult(
        "Filesystem   Size   Used   Avail  Use%  Mounted on\n"
        f"sandbox       {u['quota_bytes'] // 1024}K  {u['used_bytes'] // 1024}K  "
        f"{(u['quota_bytes'] - u['used_bytes']) // 1024}K  {pct:>3}%  /"
    )


def _c_du(s, args, stdin):
    u = s.kernel.vfs.usage()
    return CommandResult(f"{u['used_bytes']}\t{s.abspath(args[0]) if args else s.cwd}")


def _c_ps(s, args, stdin):
    procs = s.kernel.process_table()
    out = ["  PID  USER    STAT  COMMAND"]
    for p in procs:
        out.append(f"{p['pid']:>5}  {p['user']:<6}  {p['state']:<4}  {p['cmd']}")
    return CommandResult("\n".join(out))


def _c_uptime(s, args, stdin):
    secs = int(time.time() - s.kernel.started_at)
    m, sec = divmod(secs, 60)
    h, m = divmod(m, 60)
    u = s.kernel.vfs.usage()
    return CommandResult(
        f"up {h}:{m:02d}:{sec:02d}, 1 user, load average: 0.00 0.00 0.00\n"
        f"disk {u['used_bytes']}/{u['quota_bytes']} bytes, {u['files']} files"
    )


def _c_env(s, args, stdin):
    return CommandResult("\n".join(f"{k}={v}" for k, v in sorted(s.env.items())))


def _c_export(s, args, stdin):
    for a in args:
        if "=" in a:
            k, _, v = a.partition("=")
            s.env[k] = _expand_vars(s, v)
    return CommandResult("")


def _c_history(s, args, stdin):
    return CommandResult("\n".join(f"{i:>4}  {c}" for i, c in enumerate(s.history, 1)))


def _c_seq(s, args, stdin):
    try:
        nums = [int(a) for a in args if not a.startswith("-")]
    except ValueError:
        return CommandResult(code=1, stderr="seq: invalid argument")
    if len(nums) == 1:
        nums.insert(0, 1)
    if len(nums) < 2:
        return CommandResult(code=1, stderr="seq: usage: seq N or seq A B")
    return CommandResult("\n".join(str(n) for n in range(nums[0], nums[1] + 1)))


def _c_sort(s, args, stdin):
    lines = stdin.splitlines()
    if "-r" in args:
        lines.sort(reverse=True)
    else:
        lines.sort()
    return CommandResult("\n".join(lines))


def _c_uniq(s, args, stdin):
    out = []
    for line in stdin.splitlines():
        if not out or out[-1] != line:
            out.append(line)
    return CommandResult("\n".join(out))


def _c_rev(s, args, stdin):
    return CommandResult("\n".join(l[::-1] for l in stdin.splitlines()))


def _c_tee(s, args, stdin):
    if args:
        try:
            s.kernel.vfs.write_text(s.abspath(args[0]), stdin)
        except VfsError as exc:
            return CommandResult(code=1, stderr=f"tee: {exc}")
    return CommandResult(stdin)


def _c_write(s, args, stdin):
    """sandbox builtin: write text to a file (handy for the terminal)."""
    if len(args) < 2:
        return CommandResult(code=1, stderr="write: usage: write FILE TEXT...")
    try:
        s.kernel.vfs.write_text(s.abspath(args[0]), " ".join(args[1:]) + "\n")
    except VfsError as exc:
        return CommandResult(code=1, stderr=f"write: {exc}")
    return CommandResult("")


def _c_clear(s, args, stdin):
    return CommandResult("\x0cclear")


def _c_echo_screen(s, args, stdin):
    return CommandResult(" ".join(args) + "\n")


def _c_neofetch(s, args, stdin):
    u = s.kernel.vfs.usage()
    return CommandResult(
        "      .--.        SandboxOS 1.0\n"
        "     |o_o |       ------------\n"
        "     |:_/ |       Kernel:   6.0-sandbox\n"
        "    //   \\ \\      User:     " + s.user + "\n"
        "   (|     | )     Shell:    /bin/sssh\n"
        "  /'\\_   _/`\\     Screen:   " + str(s.kernel.screen.width) + "x" + str(s.kernel.screen.height) + "\n"
        "  \\___)=(___/     Disk:     " + f"{u['used_bytes']}/{u['quota_bytes']} bytes\n"
        "                  Windows:  " + str(len(s.kernel.screen.windows)) + " open\n"
        "                  Host:     none (fully simulated)"
    )


def _c_shutdown(s, args, stdin):
    return CommandResult("init: this sandbox cannot be shut down from inside; the host controls it.")


COMMANDS = {
    "help": _c_help,
    "?": _c_help,
    "pwd": _c_pwd,
    "cd": _c_cd,
    "ls": _c_ls,
    "dir": _c_ls,
    "cat": _c_cat,
    "echo": _c_echo_screen,
    "printf": _c_echo_screen,
    "mkdir": _c_mkdir,
    "touch": _c_touch,
    "rm": _c_rm,
    "cp": _c_cp,
    "mv": _c_mv,
    "head": _c_head,
    "tail": _c_tail,
    "wc": _c_wc,
    "grep": _c_grep,
    "find": _c_find,
    "tree": _c_tree,
    "stat": _c_stat_cmd,
    "date": _c_date,
    "whoami": _c_whoami,
    "id": _c_id,
    "hostname": _c_hostname,
    "uname": _c_uname,
    "df": _c_df,
    "du": _c_du,
    "ps": _c_ps,
    "uptime": _c_uptime,
    "env": _c_env,
    "export": _c_export,
    "history": _c_history,
    "seq": _c_seq,
    "sort": _c_sort,
    "uniq": _c_uniq,
    "rev": _c_rev,
    "tee": _c_tee,
    "write": _c_write,
    "clear": _c_clear,
    "neofetch": _c_neofetch,
    "shutdown": _c_shutdown,
    "reboot": _c_shutdown,
    "sudo": lambda s, a, i: CommandResult(
        "sudo: there is no host to escalate to. SandboxOS has no root outside itself."
    ),
    "exit": lambda s, a, i: CommandResult("exit: the sandbox console window cannot be closed from inside."),
}


# --------------------------------------------------------------------------
# parser / executor
# --------------------------------------------------------------------------
def _split_top_level(line: str) -> list[tuple[str, str]]:
    """Split into (operator, segment) pairs on ; && || respecting quotes."""
    parts: list[tuple[str, str]] = []
    buf = ""
    i = 0
    quote = None
    while i < len(line):
        ch = line[i]
        if quote:
            buf += ch
            if ch == quote:
                quote = None
            i += 1
            continue
        if ch in "'\"":
            quote = ch
            buf += ch
            i += 1
            continue
        two = line[i : i + 2]
        if two in ("&&", "||"):
            if buf.strip():
                parts.append((";", buf))
            parts.append((two, ""))
            buf = ""
            i += 2
            continue
        if ch in ";|\n":
            if buf.strip():
                parts.append(("|" if ch == "|" else ";", buf))
            buf = ""
            i += 1
            continue
        buf += ch
        i += 1
    if buf.strip():
        parts.append((";", buf))
    return parts


def _split_redirect(segment: str) -> tuple[str, str | None, bool]:
    quote = None
    for i, ch in enumerate(segment):
        if quote:
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
            continue
        if ch == ">":
            append = segment[i : i + 2] == ">>"
            start = i + 2 if append else i + 1
            target = segment[start:].strip()
            return segment[:i], target, append
    return segment, None, False


def run_line(session: ShellSession, line: str) -> CommandResult:
    """Execute one command line, honouring pipes, redirection and && / ||."""
    line = line.strip()
    if not line:
        return CommandResult("")
    session.history.append(line)

    parts = _split_top_level(line)
    if not parts:
        return CommandResult("")

    last = CommandResult("")
    pipeline: list[str] = []
    outs: list[str] = []
    errs: list[str] = []
    run = True
    for op, seg in parts:
        if op in ("&&", "||"):
            if pipeline:
                last = _run_pipeline(session, pipeline, "")
                pipeline = []
                outs.append(last.out)
                errs.append(last.stderr)
            run = (op == "&&" and last.code == 0) or (op == "||" and last.code != 0)
            continue
        if op == "|":
            pipeline.append(seg)
            continue
        if not run:
            run = True
            continue
        if pipeline:
            pipeline.append(seg)
            last = _run_pipeline(session, pipeline, "")
            pipeline = []
        else:
            last = _run_pipeline(session, [seg], "")
        outs.append(last.out)
        errs.append(last.stderr)
    if pipeline:  # line ended mid-pipeline
        last = _run_pipeline(session, pipeline, "")
        outs.append(last.out)
        errs.append(last.stderr)
    # stdout from every command in the sequence reaches the terminal, like a real shell
    return CommandResult("\n".join(o for o in outs if o),
                         code=last.code,
                         stderr="\n".join(e for e in errs if e))


def _run_pipeline(session: ShellSession, segments: list[str], stdin: str) -> CommandResult:
    data = stdin
    result = CommandResult("")
    for seg in segments:
        cmd_part, redirect, append = _split_redirect(seg)
        try:
            tokens = tokenize(cmd_part)
        except ShellSyntaxError as exc:
            return CommandResult(code=2, stderr=f"syntax error: {exc}")
        if not tokens:
            continue
        result = execute(session, tokens, data)
        if redirect is not None:
            target = session.abspath(_expand_vars(session, redirect.strip()))
            try:
                session.kernel.vfs.write_text(target, result.out, append=append)
            except VfsError as exc:
                return CommandResult(code=1, stderr=f"redirect: {exc}")
            result = CommandResult("", code=result.code)
        data = result.out
    return result


def execute(session: ShellSession, tokens: list[str], stdin: str = "") -> CommandResult:
    name = tokens[0]
    args: list[str] = []
    for tok in tokens[1:]:
        args.extend(_expand(session, tok))
    fn = COMMANDS.get(name)
    if fn is None:
        return CommandResult(
            code=127,
            stderr=(
                f"{name}: command not found (SandboxOS has no host binaries; "
                "type 'help' for the built-in set)"
            ),
        )
    try:
        res = fn(session, args, stdin)
    except VfsError as exc:
        return CommandResult(code=1, stderr=f"{name}: {exc}")
    except Exception as exc:  # never let the sandbox die on a bad command
        return CommandResult(code=1, stderr=f"{name}: internal error: {exc}")
    if len(res.out) > MAX_OUTPUT_CHARS:
        res.out = res.out[:MAX_OUTPUT_CHARS] + "\n... (output truncated)"
    return res
