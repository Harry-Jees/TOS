#!/usr/bin/env bash
# Build Hawk OS.
#
# Uses the portable Zig toolchain shipped in ../toolchains when it is there
# (no admin rights, no installer), otherwise falls back to g++/clang++ on PATH.
set -euo pipefail
cd "$(dirname "$0")"

ZIG_BIN="${ZIG_BIN:-../toolchains/zig/zig.exe}"
if [ -x "$ZIG_BIN" ]; then
  ZIG_BIN="$(cd "$(dirname "$ZIG_BIN")" && pwd)/$(basename "$ZIG_BIN")"
  USING_ZIG=1
  CXX=("$ZIG_BIN" c++)
  export ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$(cd .. && pwd)/toolchains/zcache}"
  mkdir -p "$ZIG_GLOBAL_CACHE_DIR"
elif command -v g++ >/dev/null 2>&1; then
  USING_ZIG=0
  CXX=(g++)
elif command -v clang++ >/dev/null 2>&1; then
  USING_ZIG=0
  CXX=(clang++)
else
  echo "No C++ compiler found. Put a compiler on PATH or place the portable toolchain in ../toolchains/zig." >&2
  exit 1
fi

SRC=(src/util.cpp src/image.cpp src/capture.cpp src/input.cpp src/winctl.cpp \
     src/agent.cpp src/ui.cpp src/main.cpp)
LIBS=(-luser32 -lgdi32 -lshell32 -lwinhttp -lmsimg32 -lcomctl32 -ldwmapi -lole32 -lwindowscodecs)

mkdir -p build
if [ "$USING_ZIG" -eq 1 ]; then
  (cd assets && "$ZIG_BIN" rc -fo ../build/hawk_res.o /:auto-includes none hawk.rc)
else
  if ! command -v windres >/dev/null 2>&1; then
    echo "windres was not found. It is required to compile the Hawk icon resource." >&2
    exit 1
  fi
  (cd assets && windres hawk.rc -O coff -o ../build/hawk_res.o)
fi

echo "building with: ${CXX[*]}"
"${CXX[@]}" -std=c++17 -O2 -Wall -Wextra -static -o hawk.exe "${SRC[@]}" build/hawk_res.o "${LIBS[@]}" \
  -Wl,--subsystem,windows
echo "built $(pwd)/hawk.exe"
