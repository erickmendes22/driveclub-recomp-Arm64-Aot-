#include "prepare.h"
#include <jni.h>
#include <vulkan/vulkan.h>
#include <string>

extern "C" JNIEXPORT jstring JNICALL
Java_org_driveclub_port_PortActivity_probeGraphics(JNIEnv* env, jclass) {
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkInstance instance;
    std::string status = "ARM64 OK. Vulkan indisponível.";
    if (vkCreateInstance(&info, nullptr, &instance) == VK_SUCCESS) {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count > 0) {
            status = "ARM64 OK. Vulkan detectou " + std::to_string(count) + " dispositivo(s).";
        }
        vkDestroyInstance(instance, nullptr);
    }
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_driveclub_port_PortActivity_inspect(JNIEnv* env, jclass, jbyteArray bytes) {
    std::vector<uint8_t> data(env->GetArrayLength(bytes));
    if (!data.empty()) {
        env->GetByteArrayRegion(bytes, 0, data.size(), reinterpret_cast<jbyte*>(data.data()));
        if (env->ExceptionCheck()) return nullptr;
    }
    const char* result = "Metadados inválidos ou sem TITLE_ID.";
    switch (InspectUpdateMetadata(data)) {
        case UpdateMetadata::driveclub_128: result = "Driveclub identificado. Atualização 1.28 confirmada pelo SFO."; break;
        case UpdateMetadata::other_version: result = "Driveclub identificado. O SFO não confirma a versão 1.28."; break;
        case UpdateMetadata::other_title: result = "O título indicado pelo SFO não é uma edição compatível de Driveclub."; break;
        case UpdateMetadata::invalid: break;
    }
    return env->NewStringUTF(result);
}
