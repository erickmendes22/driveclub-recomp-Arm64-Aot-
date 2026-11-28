// Load the exact game library extracted from the APK. Cases are generated only
// from the port's public provider declarations, not from a game executable or
// import inventory. This checks real Bionic dependencies and HLE registration
// without game files, Vulkan initialization or replacement system libraries.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include "fixtures/runtime_provider_cases.h"

#if !defined(__ANDROID__) || !defined(__aarch64__)
#error "Build this test for Android ARM64 and run it with the real Bionic runtime"
#endif

namespace {
using Start = void (*)(std::uint64_t);
using Register = void (*)();
using Resolve = std::uintptr_t (*)(const char*, int);
using SymbolCount = unsigned (*)();

template <typename T>
T Symbol(void* library, const char* name) {
    dlerror();
    void* value = dlsym(library, name);
    if (const char* error = dlerror()) {
        std::fprintf(stderr, "FAIL: dlsym(%s): %s\n", name, error);
        return nullptr;
    }
    return reinterpret_cast<T>(value);
}

unsigned Threads() {
    DIR* directory = opendir("/proc/self/task");
    if (!directory) return 0;
    unsigned count = 0;
    while (const dirent* entry = readdir(directory)) {
        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') ++count;
    }
    closedir(directory);
    return count;
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::fprintf(stderr, "Usage: %s [libdriveclub_game.so]\n", argv[0]);
        return 2;
    }
    const char* filename = argc == 2 ? argv[1] : "libdriveclub_game.so";
    void* library = dlopen(filename, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "FAIL: dlopen(%s) with actual APK/system libraries: %s\n", filename, dlerror());
        return 1;
    }
    const auto start = Symbol<Start>(library, "runtime_start");
    const auto resolve = Symbol<Resolve>(library, "runtime_resolve");
    const auto register_libraries = Symbol<Register>(library, "bbgpu_register_libraries");
    const auto symbol_count = Symbol<SymbolCount>(library, "bbgpu_symbol_count");
    if (!start || !resolve || !register_libraries || !symbol_count) return 1;

    const unsigned threads_before = Threads();
    start(1);
    register_libraries();
    const unsigned registered_once = symbol_count();
    register_libraries(); // The same registration is safe before a later runtime launch.
    if (!registered_once || symbol_count() != registered_once) {
        std::fputs("FAIL: pure library registration is empty or duplicates symbols\n", stderr);
        return 1;
    }
    const unsigned threads_after = Threads();
    if (!threads_before || threads_after != threads_before) {
        std::fprintf(stderr, "FAIL: pure import registration changed thread count (%u -> %u)\n",
                     threads_before, threads_after);
        return 1;
    }

    static_assert(RUNTIME_PROVIDER_CASE_COUNT > 0, "The public provider fixture must not be empty");
    unsigned resolved = 0, data = 0, failures = 0;
    for (const auto& item : runtime_provider_cases) {
        const auto address = resolve(item.name, item.is_data);
        const char* symbol = item.symbol && *item.symbol ? item.symbol : "unnamed public provider";
        if (address) {
            ++resolved;
            if (item.is_data) ++data;
        } else {
            ++failures;
            std::fprintf(stderr, "FAIL: declared public provider did not resolve: %s [%s], %s\n",
                         symbol, item.name, item.is_data ? "data" : "function");
        }
    }
    if (resolve("!!!!!!!!!!!#unknown#unknown", 0) || resolve("!!!!!!!!!!!#unknown#unknown", 1)) {
        std::fputs("FAIL: unknown import resolved to a fabricated provider\n", stderr);
        ++failures;
    }
    std::printf("Linked public providers: declared=%zu resolved=%u functions=%u data=%u "
                "registered_library_symbols=%u registration_failures=%u\n",
                static_cast<size_t>(RUNTIME_PROVIDER_CASE_COUNT), resolved, resolved - data, data,
                registered_once, failures);
    if (failures) return 1;
    std::puts("PASS: declared public providers resolve in the actual ARM64 Bionic APK library; "
              "this is a registration test, not a complete game-import inventory or gameplay test.");
    // Keep the real game library loaded until process exit. Its global teardown
    // is not part of import registration, and no guest/runtime session was run.
    return 0;
}
