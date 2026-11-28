#include "prepare.h"
#include "vulkan_driver.h"
#include <jni.h>
#if defined(__aarch64__)
#include "guest_cpu.h"
// Take a strong reference to the backend, so the Android linker resolves the
// Driveclub call ABI (including floating-point arguments) when loading JNI.
extern "C" decltype(&guest_cpu_call_xmm) const driveclub_fex_call_xmm = guest_cpu_call_xmm;
#endif
#include <vulkan/vulkan.h>
#include <exception>
#include <stdexcept>
#include <string>

extern "C" JNIEXPORT jstring JNICALL
Java_org_driveclub_port_PortActivity_probeGraphics(JNIEnv* env, jclass) {
    return env->NewStringUTF(driveclub_vk_probe());
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

extern "C" JNIEXPORT jstring JNICALL
Java_org_driveclub_port_LocalGame_prepareNative(JNIEnv* env, jclass, jstring path, jobject progress) {
    const char* raw = env->GetStringUTFChars(path, nullptr);
    if (!raw) return nullptr;
    std::filesystem::path root(raw);
    env->ReleaseStringUTFChars(path, raw);
    jmethodID update = nullptr;
    if (progress) {
        jclass cls = env->GetObjectClass(progress);
        update = env->GetMethodID(cls, "update", "(ILjava/lang/String;)V");
        env->DeleteLocalRef(cls);
        if (!update || env->ExceptionCheck()) return nullptr;
    }
    try {
        const auto error = PrepareGame(root / "update", root / "base", root / "work",
                root / "patches.xml", false, [&](int percent, const std::string& message) {
                    if (!progress) return;
                    jstring text = env->NewStringUTF(message.c_str());
                    if (!text) throw std::runtime_error("Falha ao comunicar progresso da preparação");
                    env->CallVoidMethod(progress, update, percent, text);
                    env->DeleteLocalRef(text);
                    if (env->ExceptionCheck()) throw std::runtime_error("Preparação interrompida");
                });
        return env->NewStringUTF(error.c_str());
    } catch (const std::exception& error) {
        if (env->ExceptionCheck()) return nullptr;
        return env->NewStringUTF(error.what());
    } catch (...) {
        if (env->ExceptionCheck()) return nullptr;
        return env->NewStringUTF("Falha nativa durante a preparação local");
    }
}
