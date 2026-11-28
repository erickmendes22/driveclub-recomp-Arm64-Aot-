// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define DRIVECLUB_VK_PUBLIC __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

// Empty driver_dir/library_name explicitly select the system driver. A custom
// request fails closed. A different selection is rejected while an instance is live.
DRIVECLUB_VK_PUBLIC int driveclub_vk_configure(const char *native_library_dir,
    const char *driver_dir, const char *library_name, const char *temporary_dir,
    char *error, size_t error_capacity);
DRIVECLUB_VK_PUBLIC int driveclub_vk_is_configured(void);
DRIVECLUB_VK_PUBLIC const char *driveclub_vk_last_error(void);
DRIVECLUB_VK_PUBLIC const char *driveclub_vk_description(void);
DRIVECLUB_VK_PUBLIC const char *driveclub_vk_probe(void);
DRIVECLUB_VK_PUBLIC PFN_vkGetInstanceProcAddr driveclub_vk_get_instance_proc_addr(void);

// Used by generated forwarders for legacy C Vulkan calls (FSR and the smoke test).
DRIVECLUB_VK_PUBLIC uint64_t driveclub_vk_generation(void);
DRIVECLUB_VK_PUBLIC PFN_vkVoidFunction driveclub_vk_lookup(const char *name);
DRIVECLUB_VK_PUBLIC void driveclub_vk_missing(const char *name) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
