#!/usr/bin/env bash
# Compile the Driveclub loader and HLE objects for Bionic prior to the renderer link.
set -euo pipefail
cd "$(dirname "$0")/../.."
tools=${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin
out=out/android-runtime
mkdir -p "$out"
for source in src/probe.c src/runtime*.c src/vulkan_smoke.c third_party/LibAtrac9/C/src/*.c; do
    "$tools/aarch64-linux-android30-clang" -std=c11 -O2 -fPIC -ffunction-sections -fdata-sections \
        -I. -Isrc -Ithird_party/SDL3-mingw/x86_64-w64-mingw32/include \
        -c "$source" -o "$out/$(basename "${source%.c}").o"
done
"$tools/llvm-ar" rcs "$out/libdriveclub-runtime.a" "$out"/*.o
