#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -B build_linux -DCMAKE_BUILD_TYPE=Release
cmake --build build_linux -j"$(nproc)"
mkdir -p build
cp -f build_linux/libtashkeel.so build/libtashkeel.so
cp -f build_linux/tashkeel_cli build/tashkeel_cli
echo "Built Linux: build/libtashkeel.so and build/tashkeel_cli"
