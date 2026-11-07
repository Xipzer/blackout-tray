#!/usr/bin/env bash
# Builds build/blackout.exe with zig (Linux, macOS or Windows via Git Bash). Needs Python 3 + Pillow for the icon.
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
mkdir -p build
python3 tools/make_icon.py build/blackout.ico
cp src/blackout.rc build/
"$ZIG" cc -target x86_64-windows-gnu -Os -s -municode -Wl,--subsystem,windows \
  src/blackout.c build/blackout.rc -o build/blackout.exe \
  -lkernel32 -luser32 -lshell32 -ladvapi32 -lsetupapi -lhid -lpowrprof -ldxva2
ls -l build/blackout.exe
