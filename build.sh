#!/usr/bin/env bash
# Builds the Zygisk module and packs the Magisk zip.
# Expects ANDROID_NDK (NDK root) in the environment and jni/zygisk.hpp present.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

VERSION="v0.4"
: "${ANDROID_NDK:?ANDROID_NDK is not set}"
CXX="${CXX:-$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++}"

[ -f jni/zygisk.hpp ] || { echo "jni/zygisk.hpp is missing"; exit 1; }
[ -f jni/aaudiocapturefix.cpp ] || { echo "jni/aaudiocapturefix.cpp is missing"; exit 1; }

STAGE="$ROOT/build/module"
rm -rf "$ROOT/build"
mkdir -p "$STAGE/zygisk"
cp -r module/. "$STAGE/"

"$CXX" -std=c++17 -O2 -fPIC -shared -Wall -Wextra \
  -fno-exceptions -fno-rtti -static-libstdc++ \
  -Wl,-z,max-page-size=16384 -Wl,--build-id=none -s \
  -I jni jni/aaudiocapturefix.cpp \
  -o "$STAGE/zygisk/arm64-v8a.so" \
  -llog -ldl

sed -i "s/^version=.*/version=$VERSION/" "$STAGE/module.prop"

ZIP="$ROOT/aaudiocapturefix-$VERSION.zip"
rm -f "$ZIP"
( cd "$STAGE" && zip -r9X "$ZIP" META-INF zygisk module.prop )

echo "Built $ZIP"
unzip -l "$ZIP"
