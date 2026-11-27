// Run against the Bionic ARM64 CPU library extracted from the actual APK.
#include "../src/guest_cpu.h"
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

extern "C" void* runtime_low_map(size_t length, int prot);

int main() {
    guest_cpu_init(runtime_low_map);
    auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* guest = static_cast<unsigned char*>(runtime_low_map(page_size, PROT_READ | PROT_WRITE));
    if (!guest) return 1;
    // movq rax,xmm0; add rax,rdi; ret (x86-64 System V).
    const unsigned char code[] = {0x66,0x48,0x0f,0x7e,0xc0,0x48,0x01,0xf8,0xc3};
    std::memcpy(guest, code, sizeof(code));
    auto fn = reinterpret_cast<uintptr_t>(guest);
    guest_cpu_code(fn, page_size);
    const uint64_t arg = 21;
    if (guest_cpu_call_xmm(fn, 1, &arg, 1, &arg) != 42) return 2;
    guest[6] = 0x29; // sub rax,rdi; ensure the previous translation is discarded.
    guest_cpu_code_changed(fn, sizeof(code));
    if (guest_cpu_call_xmm(fn, 1, &arg, 1, &arg) != 0) return 3;
    std::puts("FEX passed x86-64 integer/XMM and code invalidation checks on Bionic ARM64.");
    return 0;
}
