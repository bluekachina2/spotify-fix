#!/bin/sh
# usage: ANDROID_NDK=/path/to/ndk ./build.sh [inline]
set -e
cd "$(dirname "$0")"
[ -f jni/zygisk.hpp ] || { echo "put zygisk.hpp (Zygisk API v4) in jni/ first"; exit 1; }
INL=OFF; [ "$1" = inline ] && INL=ON
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DUSE_INLINE_HOOK=$INL
cmake --build build
rm -rf out && mkdir -p out/zygisk && cp -r template/* out/
cp build/libaaudiocapturefix.so out/zygisk/arm64-v8a.so
(cd out && zip -r ../aaudiocapturefix-v0.2.zip .)
echo "built aaudiocapturefix-v0.2.zip"
