#include <jni.h>
#include <cstdint>
#include <cstring>

extern "C" {
using PkgProgress = bool (*)(void*, std::uint64_t, std::uint64_t, const char*);
char* driveclub_pkg_inspect(int fd);
char* driveclub_pkg_extract(int fd, const char* staging, const char* fingerprint,
                           void* opaque, PkgProgress progress);
void driveclub_pkg_free(char* result);
}

namespace {
jstring utf8(JNIEnv* env, const char* text) {
    // Rust emits standard UTF-8; JNI NewStringUTF expects modified UTF-8 and
    // can abort under CheckJNI for valid supplementary Unicode characters.
    const auto size = std::strlen(text);
    if (size > 0x7fffffff) return nullptr;
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(size));
    if (!bytes) return nullptr;
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(text));
    jclass cls = env->ExceptionCheck() ? nullptr : env->FindClass("java/lang/String");
    jmethodID constructor = cls ? env->GetMethodID(cls, "<init>", "([BLjava/lang/String;)V") : nullptr;
    jstring charset = constructor ? env->NewStringUTF("UTF-8") : nullptr;
    auto result = charset ? static_cast<jstring>(env->NewObject(cls, constructor, bytes, charset)) : nullptr;
    if (charset) env->DeleteLocalRef(charset);
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(bytes);
    return result;
}

struct Progress {
    JNIEnv* env;
    jobject listener;
    jmethodID update;
};

bool progress(void* opaque, std::uint64_t done, std::uint64_t total, const char* path) {
    auto& callback = *static_cast<Progress*>(opaque);
    if (!callback.listener) return true;
    if (callback.env->ExceptionCheck()) return false;
    jstring text = utf8(callback.env, path);
    if (!text) return false;
    const bool keep_going = callback.env->CallBooleanMethod(callback.listener, callback.update,
        static_cast<jlong>(done), static_cast<jlong>(total), text) == JNI_TRUE;
    callback.env->DeleteLocalRef(text);
    return !callback.env->ExceptionCheck() && keep_going;
}

jstring result(JNIEnv* env, char* data) {
    jstring output = nullptr;
    if (!env->ExceptionCheck() && data) output = utf8(env, data);
    driveclub_pkg_free(data);
    return output;
}
}

extern "C" JNIEXPORT jstring JNICALL Java_org_driveclub_port_PkgInstaller_inspectNative(
        JNIEnv* env, jclass, jint fd) {
    return result(env, driveclub_pkg_inspect(fd));
}

extern "C" JNIEXPORT jstring JNICALL Java_org_driveclub_port_PkgInstaller_extractNative(
        JNIEnv* env, jclass, jint fd, jstring staging, jstring fingerprint, jobject listener) {
    if (!staging || !fingerprint) return nullptr;
    Progress callback{env, listener, nullptr};
    if (listener) {
        jclass cls = env->GetObjectClass(listener);
        if (!cls) return nullptr;
        callback.update = env->GetMethodID(cls, "update", "(JJLjava/lang/String;)Z");
        env->DeleteLocalRef(cls);
        if (!callback.update) return nullptr;
    }
    const char* path = env->GetStringUTFChars(staging, nullptr);
    if (!path) return nullptr;
    const char* identity = env->GetStringUTFChars(fingerprint, nullptr);
    if (!identity) {
        env->ReleaseStringUTFChars(staging, path);
        return nullptr;
    }
    char* extracted = driveclub_pkg_extract(fd, path, identity, &callback, progress);
    env->ReleaseStringUTFChars(fingerprint, identity);
    env->ReleaseStringUTFChars(staging, path);
    return result(env, extracted);
}
