#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
  echo "Please set ANDROID_NDK_HOME=/path/to/android-ndk"
  exit 1
fi

TOOLCHAIN="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
STRIP_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
API_LEVEL="${ANDROID_API:-24}"

CMAKE_GEN_ARGS=()
if command -v ninja >/dev/null 2>&1; then
  CMAKE_GEN_ARGS=(-G Ninja)
fi

for ABI in arm64-v8a armeabi-v7a x86_64; do
  OUT="build_android/$ABI"
  cmake -B "$OUT" "${CMAKE_GEN_ARGS[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API_LEVEL" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DBARQ_BUILD_CLI=OFF \
    -DBARQ_BUILD_JNI=ON
  cmake --build "$OUT" -j"$(nproc)"
  mkdir -p "android/jniLibs/$ABI"
  cp -f "$OUT/libbarq.so" "android/jniLibs/$ABI/libbarq.so"
  if [ -x "$STRIP_BIN" ]; then
    "$STRIP_BIN" --strip-unneeded "android/jniLibs/$ABI/libbarq.so"
  fi
done

echo "Built Android .so libraries under android/jniLibs/{arm64-v8a,armeabi-v7a,x86_64}/libbarq.so"
