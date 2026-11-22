#!/bin/sh
# Cross-compile 64-bit Windows barq.dll, barq.lib, and barq.exe from Linux or macOS.
# Needs zig: pip install ziglang
set -eu
cd "$(dirname "$0")/.."
mkdir -p windows
python3 -m ziglang c++ -target x86_64-windows-gnu -O3 -std=c++17 \
  -DBARQ_EXPORTS -DTASHKEEL_EXPORTS -Icpp/include -c cpp/src/diacritizer.cpp -o windows/diacritizer.obj
python3 -m ziglang c++ -target x86_64-windows-gnu -O3 -std=c++17 -shared \
  -DBARQ_EXPORTS -DTASHKEEL_EXPORTS -Icpp/include windows/diacritizer.obj cpp/src/c_api.cpp \
  -Wl,--out-implib,windows/barq.lib -o windows/barq.dll
python3 -m ziglang c++ -target x86_64-windows-gnu -O3 -std=c++17 \
  -Icpp/include windows/diacritizer.obj cpp/src/cli.cpp -o windows/barq.exe
rm -f windows/diacritizer.obj windows/barq.pdb
echo "Built Windows: windows/barq.dll, windows/barq.lib, windows/barq.exe"
