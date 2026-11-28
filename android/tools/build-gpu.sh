#!/usr/bin/env bash
# Compile Driveclub's actual GNM/VideoOut/Vulkan renderer for Android ARM64.
# The APK build can instead add_subdirectory(gpu) and link bbgpu transitively.
set -Eeuo pipefail
trap 'printf "Driveclub Android GPU build failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2' ERR
cd "$(dirname "$0")/../.."
ndk=${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r29}
root=${DRIVECLUB_GPU_ROOT:-$PWD/out/android-gpu}
[[ -f "$root/prefix/.done-ffmpeg-7.1.5" ]] || { echo 'Run android/tools/fetch-gpu-deps.sh first' >&2; exit 1; }
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH CMAKE_PREFIX_PATH
cmake -S gpu -B "$root/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-${DRIVECLUB_ANDROID_API:-30}" \
    -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release -DBB_LTO=OFF -DBB_PGO=off \
    -DDRIVECLUB_ANDROID_DEPS="$root/prefix"
if [[ ${1:-} == --configure-only ]]; then exit 0; fi
cmake --build "$root/build" --target bbgpu --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
