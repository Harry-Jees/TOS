@echo off
rem Build Hawk OS on Windows. Prefer a real portable Zig toolchain, otherwise
rem use g++ on PATH. Git LFS pointer text is not an executable toolchain.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "SRC=src\util.cpp src\image.cpp src\capture.cpp src\input.cpp src\winctl.cpp src\agent.cpp src\uia.cpp src\ui.cpp src\main.cpp"
set "LIBS=-luser32 -lgdi32 -lshell32 -lwinhttp -lmsimg32 -lcomctl32 -ldwmapi -lole32 -lwindowscodecs -lcrypt32 -luiautomationcore -loleaut32"
if not exist build mkdir build

set "USE_ZIG=0"
if exist "..\toolchains\zig\zig.exe" (
  findstr /b /c:"version https://git-lfs.github.com/spec/v1" "..\toolchains\zig\zig.exe" >nul
  if errorlevel 1 set "USE_ZIG=1"
)
if "%USE_ZIG%"=="1" goto use_zig
goto use_gpp

:use_zig
set "ZIG_GLOBAL_CACHE_DIR=%~dp0..\toolchains\zcache"
echo building with ..\toolchains\zig\zig.exe
pushd assets
"..\..\toolchains\zig\zig.exe" rc -fo ..\build\hawk_res.o /:auto-includes none hawk.rc
if errorlevel 1 goto build_failed
popd
"..\toolchains\zig\zig.exe" c++ -std=c++17 -O2 -Wall -Wextra -static -o hawk.exe %SRC% build\hawk_res.o %LIBS% -Wl,--subsystem,windows
if errorlevel 1 goto build_failed
goto success

:use_gpp
where g++ >nul 2>nul
if errorlevel 1 (
  echo No working Zig toolchain or g++ found. Install a C++17 Windows toolchain.
  exit /b 1
)
where windres >nul 2>nul
if errorlevel 1 (
  echo windres was not found. It is required to compile the Hawk icon resource.
  exit /b 1
)
echo building with g++
pushd assets
windres hawk.rc -O coff -o ..\build\hawk_res.o
if errorlevel 1 goto build_failed
popd
g++ -std=c++17 -O2 -Wall -Wextra -static -mwindows -o hawk.exe %SRC% build\hawk_res.o %LIBS%
if errorlevel 1 goto build_failed
goto success

:success
echo built %~dp0hawk.exe
echo run hawk.exe (no arguments) for the app window, or hawk.exe --help for the CLI
exit /b 0

:build_failed
echo build failed
exit /b 1
