#include "runtime_android.h"
#include "gpu/bbgpu.h"
#include <android/native_window_jni.h>
#include <jni.h>
#include "vulkan_driver.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" int driveclub_runtime_main(int argc, char **argv);
extern "C" void runtime_capture_set_screenshot_hook(int (*hook)(int enabled));

namespace {
JavaVM *vm;
jobject files;
jmethodID open_file, stat_file, list_files, resolve_file;
jclass missing_file, denied_file;
std::atomic<bool> started{false};
thread_local unsigned java_callback_depth;
std::mutex screenshot_mutex;
jobject screenshot_activity;
jmethodID apply_screenshot_policy;

class Env {
    JNIEnv *env_ = nullptr;
    bool attached_ = false;
public:
    Env() {
        ++java_callback_depth;
        if (!vm) return;
        if (vm->GetEnv(reinterpret_cast<void **>(&env_), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&env_, nullptr) == JNI_OK) attached_ = true;
            else env_ = nullptr;
        }
    }
    ~Env() { if (attached_) vm->DetachCurrentThread(); --java_callback_depth; }
    JNIEnv *get() const { return env_; }
};

int screenshot_policy(int enabled) {
    Env attached;
    JNIEnv *env = attached.get();
    if (!env) return -1;
    jobject activity = nullptr;
    jmethodID apply = nullptr;
    {
        std::lock_guard lock(screenshot_mutex);
        if (screenshot_activity) {
            activity = env->NewLocalRef(screenshot_activity);
            apply = apply_screenshot_policy;
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (activity) env->DeleteLocalRef(activity);
        return -1;
    }
    if (!activity || !apply) {
        if (activity) env->DeleteLocalRef(activity);
        return -1;
    }
    // No native mutex crosses the call: Java waits for the UI to acknowledge the
    // window policy. The local reference survives concurrent hook retirement.
    const jboolean applied = env->CallBooleanMethod(activity, apply, enabled ? JNI_TRUE : JNI_FALSE);
    const bool failed = env->ExceptionCheck();
    if (failed) env->ExceptionClear();
    env->DeleteLocalRef(activity);
    if (failed || applied != JNI_TRUE) {
        std::fprintf(stderr, "Android: screenshot window policy was not applied (%s)\n",
                     enabled ? "enable" : "disable");
        return -1;
    }
    return 0;
}

class ScreenshotBinding {
    JNIEnv *env_;
    bool installed_ = false;
public:
    explicit ScreenshotBinding(JNIEnv *env) : env_(env) {}
    int initialize(jobject activity) {
        if (!activity) return EINVAL;
        jclass cls = env_->GetObjectClass(activity);
        jmethodID apply = cls ? env_->GetMethodID(cls, "applyScreenshotPolicy", "(Z)Z") : nullptr;
        if (cls) env_->DeleteLocalRef(cls);
        if (env_->ExceptionCheck() || !apply) {
            env_->ExceptionClear();
            return EINVAL;
        }
        jobject reference = env_->NewGlobalRef(activity);
        if (env_->ExceptionCheck() || !reference) {
            env_->ExceptionClear();
            if (reference) env_->DeleteGlobalRef(reference);
            return ENOMEM;
        }
        {
            std::lock_guard lock(screenshot_mutex);
            screenshot_activity = reference;
            apply_screenshot_policy = apply;
        }
        runtime_capture_set_screenshot_hook(screenshot_policy);
        installed_ = true;
        return 0;
    }
    ~ScreenshotBinding() {
        if (!installed_) return;
        runtime_capture_set_screenshot_hook(nullptr);
        jobject reference;
        {
            std::lock_guard lock(screenshot_mutex);
            reference = screenshot_activity;
            screenshot_activity = nullptr;
            apply_screenshot_policy = nullptr;
        }
        if (reference) env_->DeleteGlobalRef(reference);
    }
};

int exception_errno(JNIEnv *env) {
    if (!env->ExceptionCheck()) return 0;
    jthrowable exception = env->ExceptionOccurred();
    env->ExceptionClear();
    int error = EIO;
    if (env->IsInstanceOf(exception, missing_file)) error = ENOENT;
    else if (env->IsInstanceOf(exception, denied_file)) error = EACCES;
    if (error != ENOENT) {
        std::fprintf(stderr, "Android: document provider failed (%s)\n", std::strerror(error));
        jclass cls = env->GetObjectClass(exception);
        jmethodID describe = cls ? env->GetMethodID(cls, "toString", "()Ljava/lang/String;") : nullptr;
        auto message = describe ? static_cast<jstring>(env->CallObjectMethod(exception, describe)) : nullptr;
        if (!env->ExceptionCheck() && message) {
            const char *text = env->GetStringUTFChars(message, nullptr);
            if (text) {
                std::fprintf(stderr, "Android: %.1024s\n", text);
                env->ReleaseStringUTFChars(message, text);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (message) env->DeleteLocalRef(message);
        if (cls) env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(exception);
    return error;
}

/* The native callbacks accept guest paths. Providers receive only validated
 * relative paths beneath the two explicitly selected trees. */
int relative_path(const char *path, std::string &relative) {
    if (!path || !*path) return ENOENT;
    if (!std::strncmp(path, "/app0", 5) && (!path[5] || path[5] == '/')) path += 5;
    else if (!std::strncmp(path, "/hostapp", 8) && (!path[8] || path[8] == '/')) path += 8;
    else if (*path == '/') return EACCES;
    while (*path == '/') ++path;
    relative.clear();
    while (*path) {
        const char *end = std::strchr(path, '/');
        if (!end) end = path + std::strlen(path);
        std::string part(path, end);
        if (part == "..") return EACCES;
        if (!part.empty() && part != ".") {
            if (!relative.empty()) relative += '/';
            relative += part;
        }
        path = *end ? end + 1 : end;
    }
    return relative.size() < 1024 ? 0 : ENAMETOOLONG;
}

std::string utf(JNIEnv *env, jstring text) {
    if (!text) return {};
    const char *bytes = env->GetStringUTFChars(text, nullptr);
    if (!bytes) return {};
    std::string result(bytes);
    env->ReleaseStringUTFChars(text, bytes);
    return result;
}

int initialize_files(JNIEnv *env, jobject provider) {
    if (!provider || env->GetJavaVM(&vm) != JNI_OK) return EINVAL;
    jclass cls = env->GetObjectClass(provider);
    open_file = env->GetMethodID(cls, "openFile", "(Ljava/lang/String;)I");
    stat_file = env->GetMethodID(cls, "statFile", "(Ljava/lang/String;)[J");
    list_files = env->GetMethodID(cls, "listFiles", "(Ljava/lang/String;)[Ljava/lang/String;");
    resolve_file = env->GetMethodID(cls, "resolveFile", "(Ljava/lang/String;)Ljava/lang/String;");
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck() || !open_file || !stat_file || !list_files || !resolve_file) return EINVAL;
    jclass missing = env->FindClass("java/io/FileNotFoundException");
    jclass denied = env->FindClass("java/lang/SecurityException");
    if (!missing || !denied) return EINVAL;
    missing_file = static_cast<jclass>(env->NewGlobalRef(missing));
    denied_file = static_cast<jclass>(env->NewGlobalRef(denied));
    files = env->NewGlobalRef(provider);
    env->DeleteLocalRef(missing);
    env->DeleteLocalRef(denied);
    return files && missing_file && denied_file ? 0 : ENOMEM;
}
}

extern "C" int driveclub_android_files_active(void) { return files != nullptr; }
extern "C" int driveclub_android_in_java(void) { return java_callback_depth != 0; }
extern "C" int driveclub_android_is_game_path(const char *path) {
    if (!files || !path || !*path) return 0;
    return *path != '/' || (!std::strncmp(path, "/app0", 5) && (!path[5] || path[5] == '/')) ||
           (!std::strncmp(path, "/hostapp", 8) && (!path[8] || path[8] == '/'));
}
extern "C" int driveclub_android_open(const char *path) {
    std::string relative;
    if (int error = relative_path(path, relative)) return -error;
    Env attached;
    JNIEnv *env = attached.get();
    if (!env || !files) return -EIO;
    jstring name = env->NewStringUTF(relative.c_str());
    if (!name) return -ENOMEM;
    const int fd = env->CallIntMethod(files, open_file, name);
    env->DeleteLocalRef(name);
    if (int error = exception_errno(env)) return -error;
    if (fd < 0) return fd; // Java may return a negative host errno directly.
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (lseek(fd, 0, SEEK_CUR) < 0) {
        const int error = errno;
        std::fprintf(stderr, "Android: %s is not seekable; select a local game folder\n", relative.c_str());
        close(fd);
        return -error;
    }
    return fd;
}
extern "C" int driveclub_android_stat(const char *path, struct stat *out) {
    if (!out) return EFAULT;
    std::string relative;
    if (int error = relative_path(path, relative)) return error;
    Env attached;
    JNIEnv *env = attached.get();
    if (!env || !files) return EIO;
    jstring name = env->NewStringUTF(relative.c_str());
    if (!name) return ENOMEM;
    auto data = static_cast<jlongArray>(env->CallObjectMethod(files, stat_file, name));
    env->DeleteLocalRef(name);
    if (int error = exception_errno(env)) return error;
    if (!data) return ENOENT;
    if (env->GetArrayLength(data) != 3) { env->DeleteLocalRef(data); return EIO; }
    jlong values[3];
    env->GetLongArrayRegion(data, 0, 3, values);
    env->DeleteLocalRef(data);
    if (int error = exception_errno(env)) return error;
    std::memset(out, 0, sizeof(*out));
    out->st_mode = (values[2] ? S_IFDIR | 0555 : S_IFREG | 0444);
    out->st_nlink = 1;
    out->st_size = values[0] > 0 ? values[0] : 0;
    out->st_blksize = 4096;
    out->st_blocks = (out->st_size + 511) / 512;
    out->st_mtim.tv_sec = values[1] / 1000;
    out->st_mtim.tv_nsec = (values[1] % 1000) * 1000000;
    out->st_atim = out->st_ctim = out->st_mtim;
    return 0;
}
extern "C" int driveclub_android_list(const char *path, char ***entries, size_t *count) {
    *entries = nullptr; *count = 0;
    std::string relative;
    if (int error = relative_path(path, relative)) return error;
    Env attached;
    JNIEnv *env = attached.get();
    if (!env || !files) return EIO;
    jstring name = env->NewStringUTF(relative.c_str());
    if (!name) return ENOMEM;
    auto data = static_cast<jobjectArray>(env->CallObjectMethod(files, list_files, name));
    env->DeleteLocalRef(name);
    if (int error = exception_errno(env)) return error;
    if (!data) return ENOENT;
    const size_t n = static_cast<size_t>(env->GetArrayLength(data));
    char **result = static_cast<char **>(std::calloc(n ? n : 1, sizeof(char *)));
    if (!result) { env->DeleteLocalRef(data); return ENOMEM; }
    int error = 0;
    for (size_t i = 0; i < n; ++i) {
        auto value = static_cast<jstring>(env->GetObjectArrayElement(data, static_cast<jsize>(i)));
        const std::string text = utf(env, value);
        if (value) env->DeleteLocalRef(value);
        if (env->ExceptionCheck()) { error = exception_errno(env); break; }
        if (text.size() < 3 || text[1] != ':' || (text[0] != 'd' && text[0] != 'f') ||
            text.find('/', 2) != std::string::npos || text.substr(2) == "..") { error = EIO; break; }
        result[i] = strdup(text.c_str());
        if (!result[i]) { error = ENOMEM; break; }
    }
    env->DeleteLocalRef(data);
    if (error) { driveclub_android_free_list(result, n); return error; }
    *entries = result; *count = n;
    return 0;
}
extern "C" void driveclub_android_free_list(char **entries, size_t count) {
    for (size_t i = 0; i < count; ++i) std::free(entries[i]);
    std::free(entries);
}
extern "C" int driveclub_android_resolve(const char *path, char *out, size_t size) {
    std::string relative;
    if (int error = relative_path(path, relative)) return error;
    Env attached;
    JNIEnv *env = attached.get();
    if (!env || !files) return EIO;
    jstring name = env->NewStringUTF(relative.c_str());
    if (!name) return ENOMEM;
    auto value = static_cast<jstring>(env->CallObjectMethod(files, resolve_file, name));
    env->DeleteLocalRef(name);
    if (int error = exception_errno(env)) return error;
    if (!value) return ENOENT;
    const std::string result = utf(env, value);
    env->DeleteLocalRef(value);
    if (int error = exception_errno(env)) return error;
    if (result.size() >= size) return ENAMETOOLONG;
    std::memcpy(out, result.c_str(), result.size() + 1);
    return 0;
}

extern "C" JNIEXPORT jint JNICALL Java_org_driveclub_port_GameActivity_runNative(
        JNIEnv *env, jobject activity, jstring prepared_dir, jstring user_dir, jstring log_path,
        jobject provider, jobject surface) {
    if (started.exchange(true)) return EALREADY;
    const std::string prepared = utf(env, prepared_dir), user = utf(env, user_dir), log = utf(env, log_path);
    if (env->ExceptionCheck()) return ENOMEM;
    if (prepared.empty() || user.empty() || log.empty() || !surface) return EINVAL;
    const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) { int error = errno; close(fd); return error; }
    if (fd > STDERR_FILENO) close(fd);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (!driveclub_vk_is_configured()) {
        std::fprintf(stderr, "Android: driver Vulkan não configurado: %s\n", driveclub_vk_last_error());
        return EINVAL;
    }
    std::fprintf(stderr, "Android: driver Vulkan selecionado: %s\n", driveclub_vk_description());
    const long host_page_size = sysconf(_SC_PAGESIZE);
    std::printf("Driveclub Android runtime: ARM64 Bionic + FEX; pid=%d page-size=%ld\n", getpid(), host_page_size);
    if (host_page_size != 4096) {
        std::fprintf(stderr,
            "Android: este aparelho usa páginas de memória de %ld bytes. O renderizador do Driveclub "
            "ainda exige páginas de 4096 bytes; a execução foi interrompida antes de iniciar a GPU. "
            "Exporte este diagnóstico em Sistema > Diagnóstico.\n"
            "Android: unsupported host page size (%ld); GPU page tracking currently requires 4 KiB. "
            "ELF alignment compatibility does not imply runtime support for 16 KiB pages.\n",
            host_page_size, host_page_size);
        return ENOTSUP;
    }
    if (int error = initialize_files(env, provider)) return error;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) return EINVAL;
    bbgpu_android_set_window(window);
    ANativeWindow_release(window);
    std::error_code ec;
    std::filesystem::create_directories(user, ec);
    if (ec) return ec.value();
    if (chdir(prepared.c_str()) != 0) return errno;
    setenv("BB_GPU_USER_DIR", user.c_str(), 1);
    setenv("BB_CRASH_DIR", user.c_str(), 1);
    std::vector<std::string> args = {
        "Driveclub Android", prepared + "/work/prep/boot-linked.bin",
        "--content-profile", prepared + "/work/prep/content.bin",
        "--patches", prepared + "/work/prep/patches.bin",
        "--app0", prepared + "/update", "--user", user,
        "--timeout", "0"
    };
    std::vector<char *> argv;
    for (auto &arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::puts("Android: document trees mounted read-only; archive reads stay on the selected device storage.");
    ScreenshotBinding screenshot_binding(env);
    if (int error = screenshot_binding.initialize(activity)) {
        std::fprintf(stderr, "Android: screenshot policy bridge initialization failed (%d)\n", error);
        return error;
    }
    const int result = driveclub_runtime_main(static_cast<int>(args.size()), argv.data());
    std::fprintf(stderr, "Android: runtime returned %d\n", result);
    return result;
}

extern "C" JNIEXPORT void JNICALL Java_org_driveclub_port_GameActivity_inputNative(
        JNIEnv *, jclass, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry, jfloat l2, jfloat r2) {
    runtime_android_pad_update(static_cast<uint32_t>(buttons), lx, ly, rx, ry, l2, r2);
}
