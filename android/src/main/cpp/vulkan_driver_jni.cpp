// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_driver.h"
#include <jni.h>
#include <exception>
#include <string>

namespace {
std::string Utf(JNIEnv *env, jstring value) {
    if (!value) return {};
    const char *text = env->GetStringUTFChars(value, nullptr);
    if (!text) return {};
    std::string result(text);
    env->ReleaseStringUTFChars(value, text);
    return result;
}
}

extern "C" JNIEXPORT jstring JNICALL Java_org_driveclub_port_DriverManager_configureNative(
    JNIEnv *env, jclass, jstring native_library_dir, jstring driver_dir,
    jstring library_name, jstring temporary_dir) {
    try {
        const auto native = Utf(env, native_library_dir), directory = Utf(env, driver_dir);
        const auto name = Utf(env, library_name), temporary = Utf(env, temporary_dir);
        if (env->ExceptionCheck()) return nullptr;
        char error[2048]{};
        driveclub_vk_configure(native.c_str(), directory.c_str(), name.c_str(), temporary.c_str(),
                              error, sizeof(error));
        return env->NewStringUTF(error);
    } catch (const std::exception& e) {
        return env->NewStringUTF(e.what());
    }
}
