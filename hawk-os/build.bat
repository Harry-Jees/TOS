@echo off
rem Build Hawk OS on Windows. Prefers the portable Zig toolchain that ships in
rem ..\toolchains (no installer, no admin rights), else falls back to g++/clang++.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "SRC=src\util.cpp src\image.cpp src\capture.cpp src\input.cpp src\winctl.cpp src\agent.cpp src\ui.cpp src\main.cpp"
set "LIBS=-luser32 -lgdi32 -lshell32 -lwinhttp -lmsimg32 -lcomctl32 -ldwmapi -lole32 -lwindowscodecs"

if exist "..\toolchains\zig\zig.exe" (
  set "ZIG_GLOBAL_CACHE_DIR=%~dp0..\toolchains\zcache"
  echo building with ..\toolchains\zig\zig.exe
  "..\toolchains\zig\zig.exe" c++ -std=c++17 -O2 -Wall -Wextra -static -o hawk.exe %SRC% %LIBS% -Wl,--subsystem,windows
) else (
  where g++ >nul 2>nul
  if errorlevel 1 (
    echo No C++ compiler found. Put g++ on PATH, or extract a portable toolchain into ..\toolchains\zig
    exit /b 1
  )
  echo building with g++
  g++ -std=c++17 -O2 -Wall -Wextra -static -mwindows -o hawk.exe %SRC% %LIBS%
)

if errorlevel 1 (
  echo build failed
  exit /b 1
)
echo built %~dp0hawk.exe
echo run hawk.exe (no arguments) for the app window, or hawk.exe --help for the CLI
