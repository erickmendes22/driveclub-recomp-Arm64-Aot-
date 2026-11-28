#!/usr/bin/env bash
# Cross-build FEXCore as a Bionic ARM64 library using GoW3's Android toolchain setup.
set -euo pipefail
cd "$(dirname "$0")/../.."
ndk=${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r29}
revision=1c4f04b71a4d33091183fd7e1a2c09e6823d27fd
source_dir=$PWD/out/android-fex/source
build_dir=$PWD/out/android-fex/build
if [[ ! -d "$source_dir/.git" ]]; then
    git init -q "$source_dir"
    git -C "$source_dir" remote add origin https://github.com/FEX-Emu/FEX.git
fi
if ! git -C "$source_dir" cat-file -e "$revision^{commit}" 2>/dev/null; then
    git -C "$source_dir" fetch --depth 1 origin "$revision"
fi
git -C "$source_dir" checkout -q --detach "$revision"
git -C "$source_dir" submodule update --init --recursive --depth 1
patch_file=$PWD/cpu/fex-host-pages.patch
if git -C "$source_dir" apply --check "$patch_file" 2>/dev/null; then
    git -C "$source_dir" apply "$patch_file"
elif ! git -C "$source_dir" apply --reverse --check "$patch_file" 2>/dev/null; then
    echo 'FEX host-page patch does not match the pinned source' >&2
    exit 1
fi
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_FEX_INCLUDE="$PWD/cpu/fex_project.cmake" \
    -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_STEAM_SUPPORT=OFF \
    -DBUILD_FEX_LINUX_TESTS=OFF -DENABLE_ZYDIS=OFF -DTUNE_ARCH=armv8.2-a -DTUNE_CPU=generic \
    -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF
cmake --build "$build_dir" --target bbcpu --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
