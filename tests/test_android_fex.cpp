// Run against the Bionic ARM64 CPU library extracted from the actual APK.
#include "../src/guest_cpu.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

extern "C" void* runtime_low_map(size_t length, int prot);

namespace {
uintptr_t nested_guest;

void handle_fault(int signal, siginfo_t* info, void* context) {
    if (guest_cpu_handle_fault(signal, info, context)) return;
    constexpr char message[] = "FEX test: unhandled native signal\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(128 + signal);
}

bool expect(const char* name, uint64_t actual, uint64_t expected) {
    if (actual == expected) {
        std::printf("PASS: %s\n", name);
        return true;
    }
    std::fprintf(stderr, "FAIL: %s: got %#llx, expected %#llx\n", name,
                 static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
    return false;
}

uint64_t bits(double value) {
    uint64_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

// All six SysV integer registers, the eight AAPCS64 integer registers, and
// stack arguments beyond both register sets must arrive in the right order.
uint64_t host_integers(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                       uint64_t e, uint64_t f, uint64_t g, uint64_t h,
                       uint64_t i, uint64_t j, uint64_t k, uint64_t l,
                       uint64_t m, uint64_t n) {
    return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i + 10*j + 11*k + 12*l + 13*m + 14*n;
}

double host_mixed(uint64_t a, uint64_t b, uint64_t c, uint64_t d, double x, double y) {
    return a + 2*b + 3*c + 4*d + 8*x + 16*y;
}

uint64_t host_callback(uint64_t value) {
    const uint64_t args[] = {value, 7};
    return guest_cpu_call(nested_guest, 2, args) + 1;
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    struct sigaction action {};
    action.sa_sigaction = handle_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (int signal : {SIGSEGV, SIGBUS, SIGILL}) {
        if (sigaction(signal, &action, nullptr) != 0) {
            std::perror("FEX test sigaction");
            return 1;
        }
    }
    guest_cpu_init(runtime_low_map);
    auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    std::printf("FEX test host page size: %zu bytes\n", page_size);
    auto* guest = static_cast<unsigned char*>(runtime_low_map(page_size, PROT_READ | PROT_WRITE));
    if (!guest || reinterpret_cast<uintptr_t>(guest) >= (uint64_t{1} << 40)
            || reinterpret_cast<uintptr_t>(guest) % page_size) return 1;
    // movq rax,xmm0; add rax,rdi; ret (x86-64 System V).
    const unsigned char code[] = {0x66,0x48,0x0f,0x7e,0xc0,0x48,0x01,0xf8,0xc3};
    std::memcpy(guest, code, sizeof(code));
    // lea rax,[rdi+rsi]; ret, called back from a native host thunk.
    const unsigned char add[] = {0x48,0x8d,0x04,0x37,0xc3};
    std::memcpy(guest + 64, add, sizeof(add));
    nested_guest = reinterpret_cast<uintptr_t>(guest + 64);
    // Align RSP, call a native host thunk, then return its XMM0 double as bits
    // in RAX. This checks both directions of the floating-point return ABI.
    unsigned char mixed[] = {0x48,0x83,0xec,0x08,0x48,0xb8,
                            0,0,0,0,0,0,0,0,0xff,0xd0,0x48,0x83,0xc4,0x08,
                            0x66,0x48,0x0f,0x7e,0xc0,0xc3};
    const auto mixed_thunk = guest_cpu_host_function(reinterpret_cast<void*>(host_mixed));
    std::memcpy(mixed + 6, &mixed_thunk, sizeof(mixed_thunk));
    std::memcpy(guest + 128, mixed, sizeof(mixed));
    auto fn = reinterpret_cast<uintptr_t>(guest);
    guest_cpu_code(fn, page_size);
    const uint64_t arg = 21;
    if (!expect("guest integer/XMM arguments", guest_cpu_call_xmm(fn, 1, &arg, 1, &arg), 42)) return 2;
    guest[6] = 0x29; // sub rax,rdi; ensure the previous translation is discarded.
    guest_cpu_code_changed(fn, sizeof(code));
    if (!expect("translated code invalidation", guest_cpu_call_xmm(fn, 1, &arg, 1, &arg), 0)) return 3;

    const auto integer_thunk = guest_cpu_host_function(reinterpret_cast<void*>(host_integers));
    std::array<uint64_t, 14> integers {};
    for (size_t i = 0; i < integers.size(); ++i) integers[i] = i + 1;
    if (!expect("native thunk with 14 integer arguments",
                guest_cpu_call(integer_thunk, integers.size(), integers.data()), 1015)) return 4;

    const uint64_t fp_args[] = {bits(2.5), bits(4.25)};
    if (!expect("native thunk with mixed integers/XMM and double return",
                guest_cpu_call_xmm(fn + 128, 4, integers.data(), 2, fp_args), bits(118.0))) return 5;

    const auto callback_thunk = guest_cpu_host_function(reinterpret_cast<void*>(host_callback));
    const uint64_t callback_arg = 34;
    if (!expect("native thunk calling back into guest code",
                guest_cpu_call(callback_thunk, 1, &callback_arg), 42)) return 6;
    guest_cpu_thread_end();
    std::puts("FEX passed the packaged Bionic ARM64 ABI and code invalidation checks.");
    return 0;
}
