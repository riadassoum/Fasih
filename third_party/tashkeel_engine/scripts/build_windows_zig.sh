#!/bin/sh
# Cross-compile 64-bit Windows barq.dll, barq.lib, and barq.exe from Linux or macOS.
# Needs zig: pip install ziglang
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/windows
python3 -m ziglang c++ -target x86_64-windows-gnu -O3 -std=c++17 \
  -DTASHKEEL_EXPORTS -Icpp/include -c cpp/src/diacritizer.cpp -o build/windows/diacritizer.obj
python3 -m ziglang c++ -target x86_64-windows-gnu -O3 -std=c++17 -shared \
  -DTASHKEEL_EXPORTS -Icpp/include build/windows/diacritizer.obj cpp/src/c_api.cpp \
  -Wl,--out-implib,build/windows/barq.lib -o build/windows/barq.dll
python3 -m ziglang c++ -target x86_64-windows-gnu -O3 -std=c++17 \
  -Icpp/include build/windows/diacritizer.obj cpp/src/cli.cpp -o build/windows/barq.exe
echo "Built Windows: build/windows/barq.dll, barq.lib, barq.exe"
