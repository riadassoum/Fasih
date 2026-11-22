#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
  echo "Please set ANDROID_NDK_HOME=/path/to/android-ndk"
  exit 1
fi

TOOLCHAIN="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
API_LEVEL="${ANDROID_API:-24}"
LLVM_STRIP="$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -maxdepth 3 -name llvm-strip 2>/dev/null | head -n 1 || true)"

for ABI in arm64-v8a armeabi-v7a x86_64; do
  OUT="build_android/$ABI"
  cmake -B "$OUT" \
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
  if [ -n "$LLVM_STRIP" ]; then
    "$LLVM_STRIP" --strip-unneeded "android/jniLibs/$ABI/libbarq.so" || true
  fi
done

rm -rf build_android
echo "Built Android .so libraries under android/jniLibs/{arm64-v8a,armeabi-v7a,x86_64}/libbarq.so"
