#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -B build_linux -DCMAKE_BUILD_TYPE=Release
cmake --build build_linux -j"$(nproc)"
mkdir -p linux
cp -f build_linux/libbarq.so linux/libbarq.so
cp -f build_linux/barq linux/barq
strip --strip-unneeded linux/libbarq.so || true
strip linux/barq || true
rm -rf build_linux
echo "Built Linux: linux/libbarq.so and linux/barq"
