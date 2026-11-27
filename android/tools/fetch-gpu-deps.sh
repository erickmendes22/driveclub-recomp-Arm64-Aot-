#!/usr/bin/env bash
# Cross-build the real Driveclub renderer dependencies for Bionic ARM64.
# Based on GoW3's tools/android/bionic/build_deps.sh; also retains Driveclub's
# network/NP and video HLE. Host tools and target libraries have separate prefixes.
set -Eeuo pipefail
trap 'printf "Driveclub GPU dependencies failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2' ERR
cd "$(dirname "$0")/../.."
project=$PWD
ndk=${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r29}
api=${DRIVECLUB_ANDROID_API:-30}
root=${DRIVECLUB_GPU_ROOT:-$project/out/android-gpu}
prefix=$root/prefix
src=$root/source
bld=$root/deps-build
host=$root/host
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-2}
tc=$ndk/toolchains/llvm/prebuilt/linux-x86_64
[[ -f "$ndk/build/cmake/android.toolchain.cmake" ]] || { echo "Invalid NDK: $ndk" >&2; exit 1; }
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH CMAKE_PREFIX_PATH
unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
mkdir -p "$prefix" "$src" "$bld" "$host/bin"

fetch() {
    local name=$1 url=$2
    [[ -f "$src/$name/.source-complete" ]] && return
    local archive=$src/$name.archive
    curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 "$url" -o "$archive"
    mkdir -p "$src/$name"
    tar --no-same-owner -xf "$archive" -C "$src/$name" --strip-components=1
    touch "$src/$name/.source-complete"
    rm -f "$archive"
}

cm() {
    local name=$1 source=$2
    shift 2
    [[ -f "$prefix/.done-$name" ]] && return
    cmake -S "$source" -B "$bld/$name" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$api" -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_FIND_ROOT_PATH="$prefix" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@" >"$bld/$name-configure.log" 2>&1 \
        || { tail -60 "$bld/$name-configure.log" >&2; exit 1; }
    cmake --build "$bld/$name" --parallel "$jobs" >"$bld/$name.log" 2>&1 \
        || { tail -60 "$bld/$name.log" >&2; exit 1; }
    cmake --install "$bld/$name" >"$bld/$name-install.log" 2>&1 \
        || { tail -60 "$bld/$name-install.log" >&2; exit 1; }
    touch "$prefix/.done-$name"
    printf 'Built Bionic dependency: %s\n' "$name"
}

# Fixed upstream releases; no rolling latest assets or host library copies.
fetch fmt-12.1.0 https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz
fetch zycore-1.5.2 https://github.com/zyantific/zycore-c/archive/refs/tags/v1.5.2.tar.gz
fetch zydis-4.1.1 https://github.com/zyantific/zydis/archive/refs/tags/v4.1.1.tar.gz
fetch xxhash-0.8.3 https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz
fetch miniz-3.1.2 https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
fetch Vulkan-Headers-1.4.363 https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.363.tar.gz
fetch vma-3.3.0 https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
fetch robin-map-1.3.0 https://github.com/Tessil/robin-map/archive/refs/tags/v1.3.0.tar.gz
fetch json-3.11.3 https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
fetch boost-1.86.0 https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2
fetch ffmpeg-7.1.5 https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz
fetch protobuf-21.12 https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.tar.gz
fetch openssl-3.5.4 https://github.com/openssl/openssl/archive/refs/tags/openssl-3.5.4.tar.gz
fetch libpng-1.6.50 https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.50.tar.gz
fetch miniupnpc-2.2.8 https://github.com/miniupnp/miniupnp/archive/b55145ec095652289a59c33603f3abafee898273.tar.gz

cm vulkan-headers-1.4.363 "$src/Vulkan-Headers-1.4.363"
mkdir -p "$prefix/include"
cp "$src/vma-3.3.0/include/vk_mem_alloc.h" "$prefix/include/"
if [[ ! -f "$prefix/include/boost/version.hpp" ]]; then
    cp -a "$src/boost-1.86.0/boost" "$prefix/include/"
fi
cm robin-map-1.3.0 "$src/robin-map-1.3.0" -DTSL_ROBIN_MAP_ENABLE_TESTS=OFF
cm json-3.11.3 "$src/json-3.11.3" -DJSON_BuildTests=OFF
cm fmt-12.1.0 "$src/fmt-12.1.0" -DFMT_TEST=OFF -DFMT_DOC=OFF
cm zycore-1.5.2 "$src/zycore-1.5.2" -DZYCORE_BUILD_TESTS=OFF
mkdir -p "$src/zydis-4.1.1/dependencies/zycore"
if [[ ! -f "$src/zydis-4.1.1/dependencies/zycore/CMakeLists.txt" ]]; then
    cp -a "$src/zycore-1.5.2/." "$src/zydis-4.1.1/dependencies/zycore/"
fi
cm zydis-4.1.1 "$src/zydis-4.1.1" -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_DOXYGEN=OFF
cm xxhash-0.8.3 "$src/xxhash-0.8.3/cmake_unofficial" -DXXHASH_BUILD_XXHSUM=OFF
cm miniz-3.1.2 "$src/miniz-3.1.2" -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DINSTALL_PROJECT=ON
cm libpng-1.6.50 "$src/libpng-1.6.50" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
cm miniupnpc-2.2.8 "$src/miniupnpc-2.2.8/miniupnpc" \
    -DUPNPC_BUILD_STATIC=ON -DUPNPC_BUILD_SHARED=OFF -DUPNPC_BUILD_TESTS=OFF -DUPNPC_BUILD_SAMPLE=OFF

if [[ ! -x "$host/bin/protoc" ]]; then
    cmake -S "$src/protobuf-21.12/cmake" -B "$bld/protobuf-host" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 \
        -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF -Dprotobuf_WITH_ZLIB=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >"$bld/protobuf-host-configure.log" 2>&1 \
        || { tail -60 "$bld/protobuf-host-configure.log" >&2; exit 1; }
    cmake --build "$bld/protobuf-host" --target protoc --parallel "$jobs" >"$bld/protobuf-host.log" 2>&1 \
        || { tail -60 "$bld/protobuf-host.log" >&2; exit 1; }
    cp "$bld/protobuf-host/protoc" "$host/bin/protoc"
fi
cm protobuf-21.12 "$src/protobuf-21.12/cmake" -DCMAKE_CXX_STANDARD=17 \
    -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF \
    -Dprotobuf_BUILD_PROTOC_BINARIES=OFF -Dprotobuf_WITH_ZLIB=OFF

# Shader compilation runs on the build host. Never execute an ARM64 glslang or
# accidentally pick a tool from the target sysroot. A supplied host binary is
# also accepted (CI may install glslang-tools instead of building it).
if [[ -n ${GLSLANG_COMPILER:-} ]]; then
    [[ -x "$GLSLANG_COMPILER" ]] || { echo "Invalid GLSLANG_COMPILER: $GLSLANG_COMPILER" >&2; exit 1; }
elif command -v glslang >/dev/null 2>&1 || command -v glslangValidator >/dev/null 2>&1; then
    : # GPU CMake's NO_CMAKE_FIND_ROOT_PATH search will resolve the host tool.
elif [[ ! -x "$host/bin/glslang" ]]; then
    fetch glslang-15.4.0 https://github.com/KhronosGroup/glslang/archive/8a85691a0740d390761a1008b4696f57facd02c4.tar.gz
    cmake -S "$src/glslang-15.4.0" -B "$bld/glslang-host" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF -DBUILD_TESTING=OFF \
        -DENABLE_GLSLANG_BINARIES=ON -DENABLE_SPVREMAPPER=OFF \
        -DALLOW_EXTERNAL_SPIRV_TOOLS=OFF >"$bld/glslang-host-configure.log" 2>&1 \
        || { tail -60 "$bld/glslang-host-configure.log" >&2; exit 1; }
    cmake --build "$bld/glslang-host" --target glslang-standalone --parallel "$jobs" \
        >"$bld/glslang-host.log" 2>&1 \
        || { tail -60 "$bld/glslang-host.log" >&2; exit 1; }
    cp "$bld/glslang-host/StandAlone/glslang" "$host/bin/glslang"
fi

if [[ ! -f "$prefix/.done-openssl-3.5.4" ]]; then
    mkdir -p "$bld/openssl-3.5.4"
    (
        cd "$bld/openssl-3.5.4"
        export PATH="$tc/bin:$PATH" ANDROID_NDK_ROOT="$ndk"
        perl "$src/openssl-3.5.4/Configure" android-arm64 "-D__ANDROID_API__=$api" \
            --prefix="$prefix" --libdir=lib no-shared no-tests no-module \
            >"$bld/openssl-configure.log" 2>&1 || exit 1
        make -j"$jobs" >"$bld/openssl.log" 2>&1 || exit 1
        make install_sw >"$bld/openssl-install.log" 2>&1 || exit 1
    ) || { tail -50 "$bld"/openssl*.log >&2; exit 1; }
    touch "$prefix/.done-openssl-3.5.4"
fi

if [[ ! -f "$prefix/.done-ffmpeg-7.1.5" ]]; then
    mkdir -p "$bld/ffmpeg-7.1.5"
    (
        cd "$bld/ffmpeg-7.1.5"
        "$src/ffmpeg-7.1.5/configure" --prefix="$prefix" --enable-cross-compile \
            --target-os=android --arch=aarch64 --cc="$tc/bin/aarch64-linux-android$api-clang" \
            --cxx="$tc/bin/aarch64-linux-android$api-clang++" --ar="$tc/bin/llvm-ar" \
            --ranlib="$tc/bin/llvm-ranlib" --strip="$tc/bin/llvm-strip" --nm="$tc/bin/llvm-nm" \
            --enable-static --disable-shared --enable-pic --disable-programs --disable-doc \
            --disable-debug --disable-avdevice --disable-avfilter --disable-network \
            --disable-autodetect --disable-mediacodec --disable-jni \
            --disable-postproc --disable-everything \
            --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
            --enable-demuxer=mov,matroska,mpegts,mp3,wav \
            --enable-decoder=h264,hevc,aac,mp3,pcm_s16le,pcm_s24le,pcm_f32le \
            --enable-parser=h264,hevc,aac,mpegaudio \
            --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc \
            --enable-protocol=file,pipe --enable-pthreads \
            >"$bld/ffmpeg-configure.log" 2>&1 || exit 1
        make -j"$jobs" >"$bld/ffmpeg.log" 2>&1 || exit 1
        make install >"$bld/ffmpeg-install.log" 2>&1 || exit 1
    ) || { tail -50 "$bld"/ffmpeg*.log >&2; exit 1; }
    # FFmpeg's Android configuration includes unused MediaCodec/JNI libraries.
    sed -i 's/ -landroid -lmediandk//' "$prefix/lib/pkgconfig/libavutil.pc"
    touch "$prefix/.done-ffmpeg-7.1.5"
fi
printf 'Driveclub Bionic GPU dependencies ready: %s\n' "$prefix"
