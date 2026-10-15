#!/bin/sh
# Cross-compiles the mod for 64-bit Windows (Far Cry 4 is x64-only).
# Needs mingw-w64:  sudo apt install gcc-mingw-w64-x86-64
set -e
cd "$(dirname "$0")"
mkdir -p dist
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-cast-function-type -shared \
    -o dist/xinput1_3.dll src/forcepad.c src/forcepad.def \
    -static-libgcc -Wl,--enable-stdcall-fixup -s -lkernel32 -luser32
cp ForcePad.ini dist/
echo "Built dist/xinput1_3.dll"
