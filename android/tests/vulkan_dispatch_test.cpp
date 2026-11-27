// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise production dispatch with independent mock loader backends. No GPU is
// required; actual Android namespaces/Turnip still require device validation.
#include "vulkan_driver.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
int system_handle, custom_handle, helper_handle;
int system_device, custom_device;
bool custom_proof = true;
unsigned system_calls{}, custom_calls{}, system_opens{}, custom_opens{};
unsigned creates{}, destroys{};
struct Instance { bool custom; };

template<typename F> PFN_vkVoidFunction Function(F function) {
    return reinterpret_cast<PFN_vkVoidFunction>(function);
}
VkResult Version(uint32_t *version) { *version = VK_API_VERSION_1_3; return VK_SUCCESS; }
template<bool Custom> VkResult Create(const VkInstanceCreateInfo *, const VkAllocationCallbacks *,
                                      VkInstance *instance) {
    *instance = reinterpret_cast<VkInstance>(new Instance{Custom});
    ++creates;
    return VK_SUCCESS;
}
void Destroy(VkInstance instance, const VkAllocationCallbacks *) {
    delete reinterpret_cast<Instance *>(instance);
    ++destroys;
}
VkResult Enumerate(VkInstance instance, uint32_t *count, VkPhysicalDevice *devices) {
    if (devices && *count)
        *devices = reinterpret_cast<VkPhysicalDevice>(reinterpret_cast<Instance *>(instance)->custom ?
            &custom_device : &system_device);
    *count = 1;
    return VK_SUCCESS;
}
void Properties(VkPhysicalDevice device, VkPhysicalDeviceProperties *properties) {
    *properties = {};
    properties->apiVersion = VK_API_VERSION_1_3;
    std::snprintf(properties->deviceName, sizeof(properties->deviceName), "%s",
        device == reinterpret_cast<VkPhysicalDevice>(&custom_device) ? "TEST custom GPU" : "TEST system GPU");
}
template<bool Custom> VkResult CreateBuffer(VkDevice, const VkBufferCreateInfo *,
                                           const VkAllocationCallbacks *, VkBuffer *buffer) {
    if constexpr (Custom) ++custom_calls; else ++system_calls;
    *buffer = reinterpret_cast<VkBuffer>(Custom ? 0x22 : 0x11);
    return VK_SUCCESS;
}
template<bool Custom> PFN_vkVoidFunction Gipa(VkInstance instance, const char *name) {
    if (!std::strcmp(name, "vkEnumerateInstanceVersion")) return Function(&Version);
    if (!std::strcmp(name, "vkCreateInstance")) return Function(&Create<Custom>);
    if (!instance) return nullptr;
    assert(reinterpret_cast<Instance *>(instance)->custom == Custom);
    if (!std::strcmp(name, "vkDestroyInstance")) return Function(&Destroy);
    if (!std::strcmp(name, "vkEnumeratePhysicalDevices")) return Function(&Enumerate);
    if (!std::strcmp(name, "vkGetPhysicalDeviceProperties")) return Function(&Properties);
    if (!std::strcmp(name, "vkCreateBuffer")) return Function(&CreateBuffer<Custom>);
    return nullptr;
}
void *OpenCustom(int, int flags, const char *, const char *, const char *directory,
                 const char *name, const char *, void **) {
    assert(flags == 1 && directory && directory[std::strlen(directory) - 1] == '/');
    assert(!std::strcmp(name, "libvulkan_freedreno.so"));
    ++custom_opens;
    return &custom_handle;
}
bool CustomLoaded(void *handle) { assert(handle == &custom_handle); return custom_proof; }

void Check(bool result, const char *description) {
    if (!result) { std::fprintf(stderr, "FAIL: %s\n", description); std::abort(); }
}
} // namespace

extern "C" void *__wrap_dlopen(const char *name, int) {
    if (!std::strcmp(name, "libvulkan.so")) { ++system_opens; return &system_handle; }
    if (std::string(name).ends_with("/libadrenotools.so")) return &helper_handle;
    return nullptr;
}
extern "C" void *__wrap_dlsym(void *handle, const char *name) {
    if (handle == &helper_handle && !std::strcmp(name, "adrenotools_open_libvulkan"))
        return reinterpret_cast<void *>(&OpenCustom);
    if (handle == &helper_handle && !std::strcmp(name, "adrenotools_custom_driver_loaded"))
        return reinterpret_cast<void *>(&CustomLoaded);
    if (!std::strcmp(name, "vkGetInstanceProcAddr")) {
        if (handle == &system_handle) return reinterpret_cast<void *>(&Gipa<false>);
        if (handle == &custom_handle) return reinterpret_cast<void *>(&Gipa<true>);
    }
    return nullptr;
}

int main() {
    char pattern[] = "/tmp/driveclub-vulkan-test-XXXXXX";
    const char *temporary = mkdtemp(pattern);
    Check(temporary != nullptr, "temporary directory");
    const std::filesystem::path root(temporary);
    for (const char *file : {"libadrenotools.so", "libhook_impl.so", "libmain_hook.so", "libvulkan_freedreno.so"})
        std::ofstream(root / file).put('\0');
    char error[2048]{};
    const auto system = [&] { return driveclub_vk_configure(temporary, "", "", temporary, error, sizeof(error)); };
    const auto custom = [&] { return driveclub_vk_configure(temporary, temporary, "libvulkan_freedreno.so",
                                                           temporary, error, sizeof(error)); };
    Check(!driveclub_vk_is_configured(), "no implicit system selection");
    Check(!driveclub_vk_get_instance_proc_addr(), "no callable loader before selection");
    Check(system() == 0 && !*error, "explicit system selection");
    Check(std::strstr(driveclub_vk_probe(), "TEST system GPU"), "probe uses selected system loader");
    Check(creates == destroys, "preflight/probe leave no live instances");

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance{};
    Check(vkCreateInstance(&info, nullptr, &instance) == VK_SUCCESS, "direct C create shares loader");
    Check(system() == 0, "same selection remains idempotent while active");
    Check(custom() == EBUSY, "cannot replace active Vulkan instance");
    Check(std::strstr(driveclub_vk_description(), "TEST system GPU"), "busy selection retains active driver");
    VkBuffer buffer{};
    Check(vkCreateBuffer(VK_NULL_HANDLE, nullptr, nullptr, &buffer) == VK_SUCCESS &&
          buffer == reinterpret_cast<VkBuffer>(0x11), "direct FSR-style call reaches system backend");
    vkDestroyInstance(instance, nullptr);

    Check(custom() == 0, "custom driver can be selected after probe/session destruction");
    Check(std::strstr(driveclub_vk_probe(), "TEST custom GPU"), "custom probe never reports system GPU");
    const auto gipa = driveclub_vk_get_instance_proc_addr();
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    Check(create(&info, nullptr, &instance) == VK_SUCCESS, "dynamic/Hpp create shares selected loader");
    Check(vkCreateBuffer(VK_NULL_HANDLE, nullptr, nullptr, &buffer) == VK_SUCCESS &&
          buffer == reinterpret_cast<VkBuffer>(0x22), "direct-call cache updates after custom selection");
    Check(system() == EBUSY, "dynamic/Hpp instances participate in lifecycle protection");
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    destroy(instance, nullptr);

    Check(system() == 0, "explicit system reselection");
    Check(vkCreateInstance(&info, nullptr, &instance) == VK_SUCCESS, "reselected system create");
    Check(vkCreateBuffer(VK_NULL_HANDLE, nullptr, nullptr, &buffer) == VK_SUCCESS &&
          buffer == reinterpret_cast<VkBuffer>(0x11), "direct-call cache returns to explicit system backend");
    vkDestroyInstance(instance, nullptr);
    const unsigned before_system = system_opens;
    custom_proof = false;
    Check(custom() == ENODEV && std::strstr(error, "bloqueado"), "valid loader with missing custom proof is rejected");
    Check(!driveclub_vk_is_configured() && !driveclub_vk_get_instance_proc_addr(), "failed custom selection disables old loader");
    Check(system_opens == before_system, "failed custom request cannot load a system fallback");
    Check(vkCreateInstance(&info, nullptr, &instance) == VK_ERROR_INITIALIZATION_FAILED,
          "game cannot start after failed custom choice");
    Check(system() == 0, "system works again only after explicit selection");
    Check(creates == destroys, "every preflight, probe and session is destroyed");
    Check(system_calls == 2 && custom_calls == 1 && custom_opens == 1 && system_opens == 1,
          "loader handles retained, direct dispatch counts exact");
    std::filesystem::remove_all(root);
    std::puts("PASS: selected-driver dispatch, direct C routing, lifecycle, cached handles and no silent fallback");
}
