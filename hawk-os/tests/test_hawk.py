#!/usr/bin/env python3
"""Automated checks for Hawk OS.

Drives the real hawk.exe and inspects what it produced. Screenshots are fully
decoded here (all five PNG filter types), and coordinates reported by the agent
are cross-checked against those decoded pixels. The mouse is put back where the
run found it.

    python tests/test_hawk.py
"""

from __future__ import annotations

import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "hawk.exe")
SHOTS = os.path.join(ROOT, "shots")
WORK = os.path.join(ROOT, "workspace")
LOG = os.path.join(ROOT, "hawk.log")

results: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    results.append((name, bool(ok), detail))
    print(f"{name:<54} {'PASS' if ok else 'FAIL'}   {detail}")


def run(*args: str, env: dict | None = None, timeout: int = 120) -> subprocess.CompletedProcess:
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    return subprocess.run([EXE, *args], capture_output=True, text=True, timeout=timeout,
                          env=full_env, errors="replace")


def cursor() -> tuple[int, int]:
    out = run("--cursor").stdout
    m = re.search(r"cursor (-?\d+) (-?\d+)", out)
    assert m, out
    return int(m.group(1)), int(m.group(2))


def desktop() -> tuple[int, int, int, int]:
    out = run("--desktop").stdout
    m = re.search(r"virtual (-?\d+) (-?\d+) (\d+) (\d+)", out)
    assert m, out
    return tuple(int(g) for g in m.groups())  # type: ignore[return-value]


def unfilter(raw: bytes, w: int, h: int, bpp: int = 4) -> bytearray:
    """Reverse PNG per-row filtering - none of the five types may be assumed."""
    stride = w * bpp
    out = bytearray(stride * h)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if ft == 1:      # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:    # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:    # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif ft != 0:
            raise ValueError(f"unknown PNG filter {ft}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def read_png(path: str):
    """Decode a PNG written by Hawk OS: (w, h, unfiltered RGBA bytes, meta)."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "bad PNG signature"
    off, idat, ihdr, chunks, palette_ok = 8, b"", None, [], True
    while off < len(data):
        ln = struct.unpack_from(">I", data, off)[0]
        typ = data[off + 4:off + 8]
        crc = struct.unpack_from(">I", data, off + 8 + ln)[0]
        assert crc == zlib.crc32(data[off + 4:off + 8 + ln]) & 0xFFFFFFFF, f"bad CRC in {typ}"
        chunks.append(typ.decode())
        if typ == b"IHDR":
            ihdr = struct.unpack_from(">IIBBBBB", data, off + 8)
        elif typ == b"IDAT":
            idat += data[off + 8:off + 8 + ln]
        off += 12 + ln
    w, h, depth, color, comp, filt, inter = ihdr
    assert depth == 8 and comp == 0 and inter == 0, f"unexpected IHDR {ihdr}"
    bpp = {6: 4, 2: 3}.get(color)
    assert bpp, f"unexpected colour type {color}"
    raw = zlib.decompress(idat)
    assert len(raw) == h * (1 + w * bpp), f"raw size {len(raw)} != {h * (1 + w * bpp)}"
    rgba = unfilter(raw, w, h, bpp)
    return w, h, rgba, {"ihdr": ihdr, "chunks": chunks, "size": len(data), "bpp": bpp}


def png_pixel(rgba: bytearray, w: int, x: int, y: int, bpp: int = 4):
    i = (y * w + x) * bpp
    # PNG stores RGB(A) in that order; the tool reports RRGGBB
    return rgba[i], rgba[i + 1], rgba[i + 2]


def hexof(px) -> str:
    return f"{px[0]:02X}{px[1]:02X}{px[2]:02X}"


def main() -> int:
    if not os.path.exists(EXE):
        print(f"hawk.exe not found at {EXE} - build it first (build.sh / build.bat)")
        return 2
    os.makedirs(SHOTS, exist_ok=True)
    print(f"Hawk OS checks using {EXE}\n")
    start_cursor = cursor()

    # 1. built-in end-to-end self-test
    st = run("--selftest")
    summary = re.search(r"(\d+)/(\d+) checks passed", st.stdout)
    check("self-test: every check passes", st.returncode == 0 and "FAIL" not in st.stdout,
          (summary.group(0) if summary else "no summary") + f", exit {st.returncode}")
    check("self-test: confirms physical input works",
          "physically drive it" in st.stdout, "")

    vx, vy, vw, vh = desktop()

    # 2. a screenshot is a real, decodable PNG of the whole desktop
    shot = run("--shot").stdout.strip().splitlines()
    path = shot[-1] if shot else ""
    check("--shot writes a file", bool(path) and os.path.exists(path), path)
    if path and os.path.exists(path):
        w, h, rgba, meta = read_png(path)
        check("screenshot dimensions match the desktop", (w, h) == (vw, vh),
              f"{w}x{h} vs {vw}x{vh}, {meta['size']} bytes, chunks {meta['chunks']}")
        check("screenshot is compressed", meta["size"] < w * h * 4 // 4,
              f"{meta['size']} bytes vs {w * h * 4} raw ({100 * meta['size'] // (w * h * 4)}%)")

    # 3. the dependency-free fallback encoder still writes a readable PNG
    plain = os.path.join(SHOTS, "plain-encoder.png")
    if os.path.exists(plain):
        os.remove(plain)
    run("--shot", plain, env={"HAWK_NO_WIC": "1"})
    ok, detail = os.path.exists(plain), "file missing"
    if ok:
        w2, h2, rgba2, meta2 = read_png(plain)
        ok = (w2, h2) == (vw, vh)
        detail = f"{w2}x{h2}, {meta2['size']} bytes, decoded and unfiltered"
    check("fallback PNG encoder produces a valid file", ok, detail)

    # 4. window enumeration sees real windows
    wins = run("--windows")
    lines = [l for l in wins.stdout.splitlines() if re.search(r"\d+\s+pid=", l)]
    check("--windows lists real windows", len(lines) >= 1, f"{len(lines)} window(s)")

    # 5. the cursor can be moved. The position is read back inside the same
    #    process (GetCursorPos right after the injection) because a live human
    #    nudging the physical mouse would override it between two CLI calls.
    target = (vx + vw // 3, vy + vh // 3)
    out = run("--run", f"move {target[0]} {target[1]}").stdout
    m = re.search(r"cursor -> (\d+),(\d+)", out)
    got = (int(m.group(1)), int(m.group(2))) if m else None
    check("a script moves the real cursor",
          got is not None and abs(got[0] - target[0]) <= 1 and abs(got[1] - target[1]) <= 1,
          f"wanted {target}, reported {got}")

    # 6. the pixel probe agrees with the decoded screenshot
    probe = (vx + vw // 2, vy + vh // 2)
    out = run("--pixel", str(probe[0]), str(probe[1])).stdout
    m = re.search(r"pixel .* = #([0-9A-Fa-f]{6})", out)
    agree, detail = False, out.strip()
    if m and path and os.path.exists(path):
        w, h, rgba, meta = read_png(path)
        want = hexof(png_pixel(rgba, w, probe[0] - vx, probe[1] - vy, meta["bpp"]))
        agree = want == m.group(1).upper()
        detail = f"probe #{m.group(1).upper()} vs decoded screenshot #{want}"
    check("--pixel agrees with the captured screenshot", agree, detail)

    # 7. find a colour that is on screen, then confirm the location is right
    find_ok, find_detail = False, "no colour sampled"
    if path and os.path.exists(path):
        w, h, rgba, meta = read_png(path)
        sx, sy = vx + vw // 4, vy + vh // 4
        want = hexof(png_pixel(rgba, w, sx - vx, sy - vy, meta["bpp"]))
        found = run("--find", want, "4").stdout
        m = re.search(r"find: #[0-9a-f]{6} at screen (-?\d+),(-?\d+)", found)
        if m:
            fx, fy = int(m.group(1)), int(m.group(2))
            back = run("--pixel", str(fx), str(fy)).stdout
            m2 = re.search(r"= #([0-9A-Fa-f]{6})", back)
            reads = m2.group(1).upper() if m2 else ""
            # The screen is alive: allow a level or two of drift per channel.
            find_ok = bool(reads) and all(
                abs(int(reads[i:i + 2], 16) - int(want[i:i + 2], 16)) <= 2 for i in (0, 2, 4))
            find_detail = f"#{want} found at {fx},{fy}, reads back #{reads or '?'}"
        else:
            find_detail = f"#{want} not located: {found.strip()[:100]}"
    check("--find locates a colour and reports the right pixel", find_ok, find_detail)

    # 8. dry run touches nothing
    probe_file = os.path.join(WORK, "dryrun.txt")
    if os.path.exists(probe_file):
        os.remove(probe_file)
    run("--dry-run", "--no-restore", "--delay", "0", "--run",
        "move 5 5\nclick left\nwrite dryrun.txt hello")
    after = cursor()
    check("dry run neither moves the cursor nor writes files",
          after != (5, 5) and not os.path.exists(probe_file),
          f"cursor {after}, file created: {os.path.exists(probe_file)}")

    # 9. real file creation through the agent
    demo_dir = os.path.join(WORK, "hawk-test")
    shutil.rmtree(demo_dir, ignore_errors=True)
    r1 = run("--run", "mkdir hawk-test")
    r2 = run("--run", "write hawk-test\\hello.txt hello from hawk os")
    created = os.path.join(demo_dir, "hello.txt")
    content = open(created, encoding="utf-8").read() if os.path.exists(created) else "<missing>"
    ok = content == "hello from hawk os"
    check("agent creates directories and files", ok and r1.returncode == 0 and r2.returncode == 0,
          created if ok else f"content was {content!r} (exit {r1.returncode}/{r2.returncode})")

    # 10. a failed check is visible in the exit code and in the output
    bad = run("--run", "verifypixel 0 0 000000 0")
    low = bad.stdout.lower()
    check("a failing verification exits non-zero",
          bad.returncode != 0 and "step failed" in low and "verifypixel" in low,
          f"exit {bad.returncode}, {[l for l in bad.stdout.splitlines() if 'fail' in l.lower()][:1]}")

    # 11. every action is recorded in hawk.log
    log_ok, log_detail = False, "no log file"
    if os.path.exists(LOG):
        text = open(LOG, encoding="utf-8", errors="replace").read()
        log_ok = "run start" in text and "hawk-test" in text and "dry-run" in text
        log_detail = f"{len(text)} bytes, {text.count(chr(10))} entries"
    check("hawk.log records the runs", log_ok, log_detail)

    # 12. without a key the goal path explains itself instead of guessing
    goal = run("--goal", "summarise what is on my screen right now")
    check("goal without a key explains the setup",
          goal.returncode != 0 and "no API key" in (goal.stdout + goal.stderr),
          (goal.stdout.strip().splitlines() or ["<empty>"])[-1][:90])

    # put the mouse back where the human left it, and confirm the position
    # in-process for the same reason as check 5
    back = run("--run", f"move {start_cursor[0]} {start_cursor[1]}").stdout
    m = re.search(r"cursor -> (\d+),(\d+)", back)
    got = (int(m.group(1)), int(m.group(2))) if m else None
    check("cursor can be put back where it started",
          got is not None and abs(got[0] - start_cursor[0]) <= 1 and abs(got[1] - start_cursor[1]) <= 1,
          f"{start_cursor} -> {got}")

    passed = sum(1 for _, ok, _ in results if ok)
    print(f"\n{passed}/{len(results)} checks passed")
    if passed != len(results):
        print("\nfailures:")
        for name, ok, detail in results:
            if not ok:
                print(f"  - {name}: {detail}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
