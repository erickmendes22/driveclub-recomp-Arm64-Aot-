// SPDX-License-Identifier: GPL-2.0-or-later
// One Vulkan loader for the launcher probe, Vulkan-Hpp/VMA, ImGui and direct FSR calls.
#include "vulkan_driver.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#if DRIVECLUB_HAS_ADRENOTOOLS
#include <adrenotools/driver.h>
#endif
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace {
struct Driver {
    std::string key;
    std::string description;
    void *loader{};
    PFN_vkGetInstanceProcAddr gipa{};
    bool (*custom_loaded)(void *){};
    bool custom{};
};
std::recursive_mutex mutex;
std::vector<std::unique_ptr<Driver>> retained;
std::unordered_map<VkInstance, Driver *> live_instances;
Driver *selected{};
std::atomic<uint64_t> generation{1};
std::string last_error = "O driver Vulkan ainda não foi configurado.";

void Report(const std::string& message) {
    std::fprintf(stderr, "Vulkan: %s\n", message.c_str());
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "DriveclubVulkan", "%s", message.c_str());
#endif
}

std::string DlError() {
    const char *message = dlerror();
    return message ? message : "o carregador não informou o motivo";
}

std::string Text(const char *value) { return value ? value : ""; }

std::string Directory(const std::string& path) {
    return !path.empty() && path.back() != '/' ? path + '/' : path;
}

bool IsDirectory(const std::string& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::path(path).is_absolute() &&
        std::filesystem::is_directory(path, error) && !error;
}

bool IsFile(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::string SafeDriverText(const char *value, size_t size) {
    std::string result(value, strnlen(value, size));
    for (char& c : result) if (static_cast<unsigned char>(c) < 32 ||
                             static_cast<unsigned char>(c) > 126) c = '?';
    return result;
}

template<typename F> F Proc(Driver& driver, VkInstance instance, const char *name) {
    return reinterpret_cast<F>(driver.gipa(instance, name));
}

uint32_t ProbeVersion(Driver& driver) {
    uint32_t version = VK_API_VERSION_1_0;
    if (auto enumerate = Proc<PFN_vkEnumerateInstanceVersion>(driver, nullptr,
                                                            "vkEnumerateInstanceVersion")) {
        if (enumerate(&version) != VK_SUCCESS) return VK_API_VERSION_1_0;
    }
    return std::min(version, uint32_t(VK_API_VERSION_1_3));
}

bool InspectInstance(Driver& driver, VkInstance instance, uint32_t api_version,
                     std::string& description, std::string& error) {
    const auto enumerate = Proc<PFN_vkEnumeratePhysicalDevices>(driver, instance,
                                                              "vkEnumeratePhysicalDevices");
    const auto properties = Proc<PFN_vkGetPhysicalDeviceProperties>(driver, instance,
                                                                  "vkGetPhysicalDeviceProperties");
    if (!enumerate || !properties) {
        error = "O carregador não fornece as consultas Vulkan necessárias.";
        return false;
    }
    uint32_t count = 0;
    VkResult result = enumerate(instance, &count, nullptr);
    // A non-null libvulkan handle is insufficient: upstream hooks may not have run.
    if (driver.custom && (!driver.custom_loaded || !driver.custom_loaded(driver.loader))) {
        error = "O driver personalizado não foi carregado; o uso do driver do sistema foi bloqueado.";
        return false;
    }
    if (result != VK_SUCCESS || count == 0) {
        error = "O driver não encontrou uma GPU compatível (vkEnumeratePhysicalDevices=" +
            std::to_string(result) + ").";
        return false;
    }
    VkPhysicalDevice physical{};
    count = 1;
    result = enumerate(instance, &count, &physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count || !physical) {
        error = "Não foi possível consultar a GPU do driver selecionado.";
        return false;
    }
    VkPhysicalDeviceProperties gpu{};
    properties(physical, &gpu);
    description = driver.custom ? "Personalizado · " : "Sistema · ";
    description += SafeDriverText(gpu.deviceName, sizeof(gpu.deviceName)) + " · Vulkan " +
        std::to_string(VK_VERSION_MAJOR(gpu.apiVersion)) + "." +
        std::to_string(VK_VERSION_MINOR(gpu.apiVersion)) + "." +
        std::to_string(VK_VERSION_PATCH(gpu.apiVersion));
    if (api_version >= VK_API_VERSION_1_2 && gpu.apiVersion >= VK_API_VERSION_1_2) {
        if (auto properties2 = Proc<PFN_vkGetPhysicalDeviceProperties2>(driver, instance,
                                                                       "vkGetPhysicalDeviceProperties2")) {
            VkPhysicalDeviceDriverProperties details{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 gpu2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            gpu2.pNext = &details;
            properties2(physical, &gpu2);
            const auto name = SafeDriverText(details.driverName, sizeof(details.driverName));
            if (!name.empty()) description += " · " + name;
        }
    }
    return true;
}

bool Validate(Driver& driver, std::string& error) {
    const auto create = Proc<PFN_vkCreateInstance>(driver, nullptr, "vkCreateInstance");
    if (!create) { error = "O carregador não exporta vkCreateInstance."; return false; }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Driveclub driver validation";
    app.apiVersion = ProbeVersion(driver);
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance{};
    const auto result = create(&info, nullptr, &instance);
    if (result != VK_SUCCESS || !instance) {
        error = "O driver selecionado não iniciou o Vulkan (vkCreateInstance=" +
            std::to_string(result) + ").";
        return false;
    }
    const auto destroy = Proc<PFN_vkDestroyInstance>(driver, instance, "vkDestroyInstance");
    if (!destroy) { error = "O carregador não exporta vkDestroyInstance."; return false; }
    const bool valid = InspectInstance(driver, instance, app.apiVersion, driver.description, error);
    destroy(instance, nullptr);
    return valid;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *instance) {
    std::lock_guard lock(mutex);
    if (!selected || !instance) return VK_ERROR_INITIALIZATION_FAILED;
    const auto create = Proc<PFN_vkCreateInstance>(*selected, nullptr, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    auto result = create(info, allocator, instance);
    if (result != VK_SUCCESS) return result;
    if (!*instance) return VK_ERROR_INITIALIZATION_FAILED;
    std::string description, error;
    const uint32_t api = info && info->pApplicationInfo ?
        info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    if (!InspectInstance(*selected, *instance, api, description, error)) {
        if (auto destroy = Proc<PFN_vkDestroyInstance>(*selected, *instance, "vkDestroyInstance"))
            destroy(*instance, allocator);
        *instance = nullptr;
        last_error = error;
        Report(error);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    live_instances.emplace(*instance, selected);
    generation.fetch_add(1, std::memory_order_release);
    Report(description);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance,
                                           const VkAllocationCallbacks *allocator) {
    std::lock_guard lock(mutex);
    if (!instance) return;
    auto found = live_instances.find(instance);
    if (found == live_instances.end()) driveclub_vk_missing("vkDestroyInstance: foreign instance");
    auto destroy = Proc<PFN_vkDestroyInstance>(*found->second, instance, "vkDestroyInstance");
    if (!destroy) driveclub_vk_missing("vkDestroyInstance");
    destroy(instance, allocator);
    live_instances.erase(found);
    generation.fetch_add(1, std::memory_order_release);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *name) {
    std::lock_guard lock(mutex);
    if (!selected || !name) return nullptr;
    if (!std::strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(&GetInstanceProcAddr);
    if (!std::strcmp(name, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance);
    if (!std::strcmp(name, "vkDestroyInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(&DestroyInstance);
    return selected->gipa(instance, name);
}

int Error(int code, const std::string& message, char *out, size_t capacity) {
    last_error = message;
    if (out && capacity) std::snprintf(out, capacity, "%s", message.c_str());
    Report(message);
    return code;
}
} // namespace

extern "C" int driveclub_vk_configure(const char *native_library_dir, const char *driver_dir,
    const char *library_name, const char *temporary_dir, char *out, size_t capacity) {
    std::lock_guard lock(mutex);
    if (out && capacity) *out = 0;
    const std::string native = Directory(Text(native_library_dir));
    const std::string directory = Directory(Text(driver_dir));
    const std::string name = Text(library_name), temporary = Text(temporary_dir);
    const bool custom = !directory.empty() || !name.empty();
    const std::string key = custom ? native + "\n" + directory + "\n" + name + "\n" + temporary : "system";
    if (selected && selected->key == key) { last_error.clear(); return 0; }
    if (!live_instances.empty())
        return Error(EBUSY, "O driver está em uso. Encerre a sessão para alterar a seleção.", out, capacity);
    // Clear the previous choice before trying a new one: a failed custom request
    // must not cause a later probe/game call to use the previous system driver.
    selected = nullptr;
    generation.fetch_add(1, std::memory_order_release);
    if (custom) {
        if (directory.empty() || name.empty() || name == "." || name == ".." ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos ||
            !name.ends_with(".so"))
            return Error(EINVAL, "O pacote não informa um nome válido de biblioteca Vulkan.", out, capacity);
        if (!IsDirectory(native) || !IsDirectory(directory) || !IsDirectory(temporary) ||
            !IsFile(directory + name))
            return Error(ENOENT, "A biblioteca Vulkan ou uma pasta privada necessária não foi encontrada.", out, capacity);
        if (directory.starts_with("/storage/") || directory.starts_with("/sdcard/"))
            return Error(EACCES, "Extraia o driver no armazenamento privado do aplicativo.", out, capacity);
        for (const char *hook : {"libadrenotools.so", "libhook_impl.so", "libmain_hook.so"})
            if (!IsFile(native + hook))
                return Error(ENOENT, std::string("Biblioteca de carregamento ausente: ") + hook +
                    ". A instalação precisa extrair as bibliotecas nativas.", out, capacity);
    }
    for (const auto& driver : retained) {
        if (driver->key == key) {
            std::string error;
            if (!Validate(*driver, error)) return Error(ENODEV, error, out, capacity);
            selected = driver.get(); last_error.clear(); return 0;
        }
    }
    auto driver = std::make_unique<Driver>();
    driver->key = key;
    driver->custom = custom;
    if (!custom) {
        driver->loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    } else {
#if DRIVECLUB_HAS_ADRENOTOOLS
        // Defer the namespace helper's constructor until a custom selection.
        void *helper = dlopen((native + "libadrenotools.so").c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!helper) return Error(ELIBACC, "Não foi possível carregar libadrenotools: " + DlError(), out, capacity);
        auto open = reinterpret_cast<decltype(&adrenotools_open_libvulkan)>(dlsym(helper, "adrenotools_open_libvulkan"));
        driver->custom_loaded = reinterpret_cast<decltype(&adrenotools_custom_driver_loaded)>(
            dlsym(helper, "adrenotools_custom_driver_loaded"));
        if (!open || !driver->custom_loaded)
            return Error(ELIBBAD, "A biblioteca de carregamento não fornece a verificação do driver selecionado.", out, capacity);
        driver->loader = open(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
            temporary.c_str(), native.c_str(), directory.c_str(), name.c_str(), nullptr, nullptr);
#else
        return Error(ENOTSUP, "Drivers personalizados estão disponíveis na versão ARM64.", out, capacity);
#endif
    }
    if (!driver->loader)
        return Error(ELIBACC, "Não foi possível abrir o carregador Vulkan selecionado: " + DlError(), out, capacity);
    driver->gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(driver->loader, "vkGetInstanceProcAddr"));
    if (!driver->gipa)
        return Error(ELIBBAD, "O carregador selecionado não exporta vkGetInstanceProcAddr.", out, capacity);
    std::string error;
    if (!Validate(*driver, error)) return Error(ENODEV, error, out, capacity);
    selected = driver.get();
    retained.emplace_back(std::move(driver));
    last_error.clear();
    Report("Seleção validada: " + selected->description);
    return 0;
}

extern "C" int driveclub_vk_is_configured(void) {
    std::lock_guard lock(mutex);
    return selected != nullptr;
}

extern "C" const char *driveclub_vk_last_error(void) {
    thread_local std::string copy;
    std::lock_guard lock(mutex);
    copy = last_error;
    return copy.c_str();
}

extern "C" const char *driveclub_vk_description(void) {
    thread_local std::string copy;
    std::lock_guard lock(mutex);
    copy = selected ? selected->description : last_error;
    return copy.c_str();
}

extern "C" const char *driveclub_vk_probe(void) {
    thread_local std::string result;
    std::lock_guard lock(mutex);
    if (!selected) {
        result = "Vulkan indisponível: " + last_error;
    } else {
        std::string error;
        result = Validate(*selected, error) ? selected->description : "Vulkan indisponível: " + error;
        if (!error.empty()) last_error = error;
    }
    return result.c_str();
}

extern "C" PFN_vkGetInstanceProcAddr driveclub_vk_get_instance_proc_addr(void) {
    std::lock_guard lock(mutex);
    return selected ? &GetInstanceProcAddr : nullptr;
}

extern "C" uint64_t driveclub_vk_generation(void) {
    return generation.load(std::memory_order_acquire);
}

extern "C" PFN_vkVoidFunction driveclub_vk_lookup(const char *name) {
    std::lock_guard lock(mutex);
    if (!selected) return nullptr;
    if (auto fn = GetInstanceProcAddr(nullptr, name)) return fn;
    if (!live_instances.empty()) return GetInstanceProcAddr(live_instances.begin()->first, name);
    return nullptr;
}

extern "C" void driveclub_vk_missing(const char *name) {
    Report(std::string("Entrada Vulkan indisponível no driver selecionado: ") + name +
        ". A chamada não foi redirecionada ao driver do sistema.");
    std::abort();
}

extern "C" DRIVECLUB_VK_PUBLIC VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateInstance(
    const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *instance) {
    return CreateInstance(info, allocator, instance);
}
extern "C" DRIVECLUB_VK_PUBLIC VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *allocator) {
    DestroyInstance(instance, allocator);
}
extern "C" DRIVECLUB_VK_PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetInstanceProcAddr(
    VkInstance instance, const char *name) {
    return GetInstanceProcAddr(instance, name);
}
