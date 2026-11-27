# All non-platform libraries are built with the same NDK/Bionic toolchain.
# This file is also used when gpu/ is a subdirectory of the Android JNI build.
set(DRIVECLUB_ANDROID_DEPS "${CMAKE_CURRENT_LIST_DIR}/../../out/android-gpu/prefix"
    CACHE PATH "Driveclub Android dependency prefix")
get_filename_component(DRIVECLUB_ANDROID_DEPS "${DRIVECLUB_ANDROID_DEPS}" ABSOLUTE)
if (NOT EXISTS "${DRIVECLUB_ANDROID_DEPS}/include/vulkan/vulkan.hpp")
    message(FATAL_ERROR "Run android/tools/fetch-gpu-deps.sh before building the Android renderer")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${DRIVECLUB_ANDROID_DEPS}")
list(PREPEND CMAKE_FIND_ROOT_PATH "${DRIVECLUB_ANDROID_DEPS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} "${DRIVECLUB_ANDROID_DEPS}/lib/pkgconfig:${DRIVECLUB_ANDROID_DEPS}/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_LIBDIR}")
unset(ENV{PKG_CONFIG_SYSROOT_DIR})

# The NDK Vulkan loader is used with newer, pinned C and Hpp headers. Do not
# accidentally select an x86-64 libvulkan from the build host.
set(Vulkan_INCLUDE_DIR "${DRIVECLUB_ANDROID_DEPS}/include" CACHE PATH "Vulkan headers" FORCE)
set(OPENSSL_ROOT_DIR "${DRIVECLUB_ANDROID_DEPS}")
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(Protobuf_PROTOC_EXECUTABLE "${DRIVECLUB_ANDROID_DEPS}/../host/bin/protoc" CACHE FILEPATH "Host protoc" FORCE)
if (DEFINED ENV{GLSLANG_COMPILER} AND NOT "$ENV{GLSLANG_COMPILER}" STREQUAL "")
    set(GLSLANG_COMPILER "$ENV{GLSLANG_COMPILER}" CACHE FILEPATH "Host GLSL compiler" FORCE)
elseif (EXISTS "${DRIVECLUB_ANDROID_DEPS}/../host/bin/glslang")
    set(GLSLANG_COMPILER "${DRIVECLUB_ANDROID_DEPS}/../host/bin/glslang" CACHE FILEPATH "Host GLSL compiler" FORCE)
endif()

# Boost is header-only in this renderer. Import only its staged headers, never
# /usr/include wholesale (which would override Bionic libc headers).
if (NOT TARGET Boost::headers)
    if (NOT EXISTS "${DRIVECLUB_ANDROID_DEPS}/include/boost/version.hpp")
        message(FATAL_ERROR "The Android dependency prefix is missing Boost headers")
    endif()
    add_library(Boost::headers INTERFACE IMPORTED)
    set_target_properties(Boost::headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${DRIVECLUB_ANDROID_DEPS}/include")
endif()
