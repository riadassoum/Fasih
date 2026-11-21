#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
  echo "Please set ANDROID_NDK_HOME=/path/to/android-ndk"
  exit 1
fi

TOOLCHAIN="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
API_LEVEL="${ANDROID_API:-24}"

for ABI in arm64-v8a armeabi-v7a x86_64; do
  OUT="build_android/$ABI"
  cmake -B "$OUT" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API_LEVEL" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTASHKEEL_BUILD_CLI=OFF \
    -DTASHKEEL_BUILD_JNI=ON
  cmake --build "$OUT" -j"$(nproc)"
  mkdir -p "android/jniLibs/$ABI"
  cp -f "$OUT/libtashkeel.so" "android/jniLibs/$ABI/libtashkeel.so"
done

echo "Built Android .so libraries under android/jniLibs/{arm64-v8a,armeabi-v7a,x86_64}/libtashkeel.so"
