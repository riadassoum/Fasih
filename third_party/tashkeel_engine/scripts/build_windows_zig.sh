#!/bin/sh
# Cross-compile 64-bit Windows barq.dll, barq.lib, and barq.exe from Linux or macOS.
# Needs zig on PATH or pip install ziglang
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/windows
if command -v zig >/dev/null 2>&1; then
  ZIG_CXX="zig c++"
else
  ZIG_CXX="python3 -m ziglang c++"
fi
$ZIG_CXX -target x86_64-windows-gnu -march=x86_64_v3 -O3 -std=c++17 \
  -DTASHKEEL_EXPORTS -Icpp/include -c cpp/src/diacritizer.cpp -o build/windows/diacritizer.obj
$ZIG_CXX -target x86_64-windows-gnu -march=x86_64_v3 -O3 -std=c++17 -shared \
  -DTASHKEEL_EXPORTS -Icpp/include build/windows/diacritizer.obj cpp/src/c_api.cpp \
  -Wl,--out-implib,build/windows/barq.lib -o build/windows/barq.dll
$ZIG_CXX -target x86_64-windows-gnu -march=x86_64_v3 -O3 -std=c++17 \
  -Icpp/include build/windows/diacritizer.obj cpp/src/cli.cpp -o build/windows/barq.exe
echo "Built Windows: build/windows/barq.dll, barq.lib, barq.exe"
