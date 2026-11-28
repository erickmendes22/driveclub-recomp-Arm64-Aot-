// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_driver.h"
#include <atomic>

// Cached pointers are invalidated whenever the loader/instance changes. No
// direct call may resolve against a different libvulkan from the renderer.
#define DRIVECLUB_VK_FORWARD(result, name, params, arguments) \
extern "C" DRIVECLUB_VK_PUBLIC VKAPI_ATTR result VKAPI_CALL __wrap_##name params { \
    static std::atomic<PFN_##name> cached{}; \
    static std::atomic<uint64_t> cached_generation{}; \
    const uint64_t current = driveclub_vk_generation(); \
    auto function = cached.load(std::memory_order_acquire); \
    if (!function || cached_generation.load(std::memory_order_acquire) != current) { \
        function = reinterpret_cast<PFN_##name>(driveclub_vk_lookup(#name)); \
        if (!function) driveclub_vk_missing(#name); \
        cached.store(function, std::memory_order_release); \
        cached_generation.store(current, std::memory_order_release); \
    } \
    return function arguments; \
}
#include "vulkan_forwarders.inc"
#undef DRIVECLUB_VK_FORWARD
