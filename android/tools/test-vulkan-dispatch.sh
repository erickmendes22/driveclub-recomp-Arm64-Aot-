#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
headers="${VULKAN_HEADERS_DIR:-${project_root}/out/android-gpu/prefix/include}"
build="${project_root}/out/android-vulkan-dispatch-test"
mkdir -p "$build"
"${CXX:-c++}" -std=c++20 -O2 -pthread -Wall -Wextra -Wno-missing-field-initializers \
    -DDRIVECLUB_HAS_ADRENOTOOLS=1 -I"$headers" \
    -I"${project_root}/android/src/main/cpp" \
    -I"${project_root}/android/third_party/adrenotools/include" \
    "${project_root}/android/src/main/cpp/vulkan_driver.cpp" \
    "${project_root}/android/src/main/cpp/vulkan_forwarders.cpp" \
    "${project_root}/android/tests/vulkan_dispatch_test.cpp" \
    -Wl,--wrap=dlopen -Wl,--wrap=dlsym -Wl,--wrap=vkCreateInstance \
    -Wl,--wrap=vkDestroyInstance -Wl,--wrap=vkCreateBuffer -ldl \
    -o "$build/vulkan-dispatch-test"
"$build/vulkan-dispatch-test"
