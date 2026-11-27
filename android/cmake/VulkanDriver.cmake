# One selected loader for every Android Vulkan consumer. Do not link libvulkan
# statically: custom drivers use libadrenotools' isolated Android loader instead.
include_guard(GLOBAL)
get_filename_component(_dc_android "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_dc_native "${_dc_android}/src/main/cpp")
set(_dc_adreno "${_dc_android}/third_party/adrenotools")

add_library(driveclub_vulkan SHARED
    "${_dc_native}/vulkan_driver.cpp" "${_dc_native}/vulkan_forwarders.cpp")
target_compile_features(driveclub_vulkan PRIVATE cxx_std_20)
target_include_directories(driveclub_vulkan PUBLIC "${_dc_native}")
target_link_libraries(driveclub_vulkan PRIVATE android log dl)
set_target_properties(driveclub_vulkan PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_link_options(driveclub_vulkan PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)

if (ANDROID_ABI STREQUAL "arm64-v8a" OR CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    add_library(driveclub_linkernsbypass STATIC
        "${_dc_adreno}/lib/linkernsbypass/android_linker_ns.cpp"
        "${_dc_adreno}/lib/linkernsbypass/elf_soname_patcher.cpp")
    set_target_properties(driveclub_linkernsbypass PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_features(driveclub_linkernsbypass PRIVATE cxx_std_20)
    target_include_directories(driveclub_linkernsbypass PUBLIC "${_dc_adreno}/lib/linkernsbypass")
    target_link_libraries(driveclub_linkernsbypass PUBLIC android dl)

    add_library(adrenotools SHARED "${_dc_adreno}/src/driver.cpp")
    target_compile_features(adrenotools PRIVATE cxx_std_20)
    target_include_directories(adrenotools PUBLIC "${_dc_adreno}/include")
    target_link_libraries(adrenotools PRIVATE driveclub_linkernsbypass)

    add_library(hook_impl SHARED "${_dc_adreno}/src/hook/hook_impl.cpp")
    target_compile_features(hook_impl PRIVATE cxx_std_20)
    target_include_directories(hook_impl PRIVATE "${_dc_adreno}/include")
    target_link_libraries(hook_impl PRIVATE driveclub_linkernsbypass log)
    set_target_properties(hook_impl PROPERTIES CXX_VISIBILITY_PRESET hidden)

    add_library(main_hook SHARED "${_dc_adreno}/src/hook/main_hook.c")
    target_link_libraries(main_hook PRIVATE hook_impl)
    target_link_options(main_hook PRIVATE "LINKER:-z,global")

    foreach(_target adrenotools hook_impl main_hook)
        target_link_options(${_target} PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384
            "LINKER:--exclude-libs,libdriveclub_linkernsbypass.a")
    endforeach()
    # Build/package helpers but dlopen them only after an explicit custom choice.
    add_dependencies(driveclub_vulkan adrenotools hook_impl main_hook)
    target_include_directories(driveclub_vulkan PRIVATE "${_dc_adreno}/include")
    target_compile_definitions(driveclub_vulkan PRIVATE DRIVECLUB_HAS_ADRENOTOOLS=1)
else()
    target_compile_definitions(driveclub_vulkan PRIVATE DRIVECLUB_HAS_ADRENOTOOLS=0)
endif()

add_library(driveclub_vulkan_dispatch INTERFACE)
target_link_libraries(driveclub_vulkan_dispatch INTERFACE driveclub_vulkan)
# Retain the modern pinned headers used by FidelityFX while replacing only its
# loader dependency. The x86 launcher can use the Vulkan headers inside the NDK.
if (DEFINED DRIVECLUB_ANDROID_DEPS)
    set(_dc_vk_headers "${DRIVECLUB_ANDROID_DEPS}/include")
else()
    set(_dc_vk_headers "${_dc_android}/../out/android-gpu/prefix/include")
endif()
if (EXISTS "${_dc_vk_headers}/vulkan/vulkan.h")
    target_include_directories(driveclub_vulkan SYSTEM PRIVATE "${_dc_vk_headers}")
    target_include_directories(driveclub_vulkan_dispatch SYSTEM INTERFACE "${_dc_vk_headers}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/VulkanCommands.cmake")
foreach(_command IN LISTS DRIVECLUB_VULKAN_COMMANDS)
    target_link_options(driveclub_vulkan_dispatch INTERFACE "LINKER:--wrap=${_command}")
endforeach()
# FindVulkan creates this target only when it does not already exist. Thus FSR
# inherits this dispatch target rather than an accidental NDK libvulkan link.
if (TARGET Vulkan::Vulkan)
    message(FATAL_ERROR "Initialize the Android Vulkan selector before FindVulkan")
endif()
add_library(Vulkan::Vulkan ALIAS driveclub_vulkan_dispatch)
