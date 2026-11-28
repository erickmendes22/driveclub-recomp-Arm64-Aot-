// Run against the Bionic ARM64 CPU library extracted from the actual APK.
#include "../src/guest_cpu.h"
#include "../src/runtime_exit.h"
#include "guest_setcontext_contract.h"
extern "C" {
#include "../src/runtime.h"
int runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
// The test supplies a synthetic image and does not perform save transactions.
uintptr_t runtime_image_base;
uint64_t runtime_image_size;
int runtime_savedata_busy(void) { return 0; }
}
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <sys/mman.h>
#include <unistd.h>

// Link only this test with --wrap=mmap. The production runtime object is
// unchanged: the wrapper reproduces a host rejecting the two high heap hints.
// It does not simulate FEX or any of the successful mappings/copies.
static bool restrict_guest_maps_to_39_bits;
static unsigned rejected_high_maps;
static uint64_t guest_bridge_calls;
extern "C" uint64_t __real_guest_cpu_call(uintptr_t, unsigned, const uint64_t*);
extern "C" uint64_t __wrap_guest_cpu_call(uintptr_t address, unsigned count, const uint64_t* args) {
    ++guest_bridge_calls;
    return __real_guest_cpu_call(address, count, args);
}
extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" void* __wrap_mmap(void* address, size_t size, int prot, int flags, int fd, off_t offset) {
    constexpr uintptr_t ceiling = uint64_t{1} << 39;
    const auto start = reinterpret_cast<uintptr_t>(address);
    if (restrict_guest_maps_to_39_bits && address && (start >= ceiling || size > ceiling - start)) {
        ++rejected_high_maps;
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return __real_mmap(address, size, prot, flags, fd, offset);
}

// Select the production memory exports by their symbol names without linking
// the game's unrelated HLE modules or requiring its import table.
extern "C" uintptr_t runtime_lookup(const RuntimeExport* table, size_t count, const char* name) {
    for (size_t i = 0; i < count; ++i)
        if (!std::strcmp(table[i].name, name)) return reinterpret_cast<uintptr_t>(table[i].function);
    return 0;
}

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

uint64_t call_memory(const char* name, std::initializer_list<uint64_t> args) {
    const auto function = runtime_memory_resolve(name);
    if (!function) return UINT64_MAX;
    const auto thunk = guest_cpu_host_function(reinterpret_cast<void*>(function));
    return guest_cpu_call(thunk, static_cast<unsigned>(args.size()), args.begin());
}

bool test_boot_heaps(unsigned char* guest) {
    constexpr uint64_t mib = 1024 * 1024, page = 16384, alignment = 2*mib;
    struct Globals { void* video; void* main; void* flex; void* magic; int64_t physical[3]; };
    auto* globals = reinterpret_cast<Globals*>(guest + 512);
    *globals = {reinterpret_cast<void*>(0x5000000000), reinterpret_cast<void*>(0x2000000000),
                reinterpret_cast<void*>(0xf000000000), reinterpret_cast<void*>(0xa000000000), {0,0,0}};
    auto pointer = [](const void* value) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)); };
    constexpr uint64_t sizes[] = {3674*mib, 530*mib, 176*mib};
    constexpr uint64_t types[] = {3, 0, 0};
    void** addresses[] = {&globals->video, &globals->main, &globals->magic};
    const auto budget = call_memory("sceKernelGetDirectMemorySize", {});
    if (!expect("unchanged direct-memory budget", budget, 5056*mib)) return false;
    restrict_guest_maps_to_39_bits = true;
    // Match the real device's order and full allocation sizes, using sparse
    // backing. The test commits only the pages it subsequently touches.
    for (unsigned i = 0; i < 3; ++i) {
        if (i == 2 && !expect("FEX flexible heap hint fallback",
                call_memory("sceKernelMapFlexibleMemory", {pointer(&globals->flex),338*mib,3,0}), 0)) return false;
        if (!expect("FEX direct allocation", call_memory("sceKernelAllocateDirectMemory",
                {0,budget,sizes[i],alignment,types[i],pointer(&globals->physical[i])}), 0)) return false;
        if (!expect("FEX direct heap mapping", call_memory("sceKernelMapDirectMemory",
                {pointer(addresses[i]),sizes[i],i == 0 ? 0x32u : i == 2 ? 0x33u : 3u,0,
                 static_cast<uint64_t>(globals->physical[i]),alignment}), 0)) return false;
        if (!expect("direct heap retains requested 2 MiB alignment",
                pointer(*addresses[i]) % alignment,0)) return false;
    }
    const auto flex = pointer(globals->flex), magic = pointer(globals->magic);
    if (!expect("both original high hints were rejected", rejected_high_maps >= 2, true)
            || !expect("valid VideoPrivate hint retained", pointer(globals->video), 0x5000000000)
            || !expect("valid MainHeap hint retained", pointer(globals->main), 0x2000000000)
            || !expect("returned FlexHeap fits 39-bit host", flex < (uint64_t{1} << 39) - 338*mib, true)
            || !expect("returned MagicMemory fits 39-bit host", magic < (uint64_t{1} << 39) - 176*mib, true)
            || !expect("complete relocated FlexHeap is mapped", runtime_memory_is_mapped(flex,338*mib), 1)
            || !expect("complete relocated MagicMemory is mapped", runtime_memory_is_mapped(magic,176*mib), 1)) return false;
    const uint64_t starts[] = {pointer(globals->video),pointer(globals->main),flex,magic};
    const uint64_t lengths[] = {sizes[0],sizes[1],338*mib,sizes[2]};
    bool disjoint = true, scratch_available = true;
    for (unsigned i = 0; i < 4; ++i) {
        scratch_available &= starts[i] >= 0x2000000000 || starts[i] + lengths[i] <= 0x1000000000;
        for (unsigned j = i + 1; j < 4; ++j)
            disjoint &= starts[i] + lengths[i] <= starts[j] || starts[j] + lengths[j] <= starts[i];
    }
    if (!expect("all four heap extents are disjoint",disjoint,true)
            || !expect("64–128 GiB scratch band remains free",scratch_available,true)) return false;

    // x86-64 code reloads *inout, adds the file_system offset from the crash,
    // calls the real Bionic memmove through a FEX thunk, then reads its result.
    unsigned char copy[] = {0x48,0x83,0xec,0x08, 0x48,0x8b,0x3f,
                            0x48,0x81,0xc7,0xcc,0x00,0x00,0x02,
                            0x48,0xb8,0,0,0,0,0,0,0,0, 0xff,0xd0,
                            0x48,0x83,0xc4,0x08, 0x48,0x8b,0x00,0xc3};
    const auto memmove_thunk = guest_cpu_host_function(reinterpret_cast<void*>(std::memmove));
    std::memcpy(copy + 16, &memmove_thunk, sizeof(memmove_thunk));
    std::memcpy(guest + 256, copy, sizeof(copy));
    guest_cpu_code_changed(reinterpret_cast<uintptr_t>(guest + 256), sizeof(copy));
    constexpr uint64_t value = 0x0123456789abcdef;
    std::memcpy(guest + 768, &value, sizeof(value));
    const uint64_t copy_args[] = {pointer(&globals->flex),pointer(guest + 768),sizeof(value)};
    if (!expect("FEX uses relocated pointer in Bionic memmove at file_system offset",
            guest_cpu_call(pointer(guest + 256),3,copy_args),value)) return false;
    uint64_t backing = 0;
    if (!expect("GPU backing view covers relocated file_system memory",
            runtime_memory_read_backing(flex + 0x020000cc,&backing,sizeof(backing)),1)
            || !expect("GPU backing view and guest agree",backing,value)) return false;

    // The fixed scratch alias at 64 GiB must still be available and must share
    // the direct allocation's physical backing with the relocated Magic heap.
    struct PrtResult { void* base; uint64_t size; };
    auto* prt = reinterpret_cast<PrtResult*>(guest + 896);
    if (!expect("FEX sets sparse PRT aperture",call_memory("sceKernelSetPrtAperture",
                {0,0x1000000000,2*mib+page}),0)
            || !expect("FEX queries sparse PRT aperture",call_memory("sceKernelGetPrtAperture",
                {0,pointer(&prt->base),pointer(&prt->size)}),0)
            || !expect("PRT returned guest base",pointer(prt->base),0x1000000000)
            || !expect("PRT returned guest length",prt->size,2*mib+page)
            || !expect("PRT does not invent CPU backing",runtime_memory_is_mapped(0x1000000000,1),0)) return false;
    auto** alias = reinterpret_cast<void**>(guest + 640);
    *alias = reinterpret_cast<void*>(0x1000000000);
    if (!expect("fixed scratch alias remains at 64 GiB",call_memory("sceKernelMapDirectMemory",
            {pointer(alias),page,3,0x10,static_cast<uint64_t>(globals->physical[2]),page}),0)
            || !expect("fixed alias address retained",pointer(*alias),0x1000000000)) return false;
    std::memcpy(globals->magic,&value,sizeof(value));
    std::memcpy(&backing,*alias,sizeof(backing));
    if (!expect("fixed scratch alias shares relocated direct backing",backing,value)) return false;
    if (!expect("FEX clears PRT coverage",call_memory("sceKernelSetPrtAperture",
                {0,0x1000000000,0}),0)
            || !expect("clearing PRT preserves physical backing",runtime_memory_is_mapped(0x1000000000,page),1)) return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!expect("FEX releases direct allocation and aliases",call_memory(i==2 ? "sceKernelCheckedReleaseDirectMemory" : "sceKernelReleaseDirectMemory",
                {static_cast<uint64_t>(globals->physical[i]),sizes[i]}),0)) return false;
    if (!expect("released fixed alias no longer mapped",runtime_memory_is_mapped(pointer(*alias),page),0)
            || !expect("FEX releases relocated flexible heap",call_memory("sceKernelReleaseFlexibleMemory",
                    {flex,338*mib}),0)) return false;
    restrict_guest_maps_to_39_bits = false;
    return true;
}

bool test_exit_guest_bridges() {
    // Only signatures checked by the production installer and small, synthetic
    // x86 functions are present. No game asset or executable is required.
    constexpr size_t image_size = 26 * 1024 * 1024;
    auto* image = static_cast<unsigned char*>(runtime_low_map(image_size, PROT_READ | PROT_WRITE));
    if (!image) return false;
    runtime_image_base = reinterpret_cast<uintptr_t>(image);
    runtime_image_size = image_size;
    constexpr size_t predicate_offset = 0x5008f0, region_offset = 0x15585c4;
    constexpr size_t destructor_offset = 0xea24d0, webapi_offset = 0x7d6e70;
    const unsigned char predicate[] = {
        0x48,0x8d,0x05,0xcd,0x7c,0x05,0x01,0x8b,0x00,0x83,0xf8,0x02,0x74,0x05,
        0x83,0xf8,0x04,0x75,0x03,0xb0,0x01,0xc3,0x83,0xf8,0x05,0x0f,0x94,0xc0,0xc3};
    const unsigned char prologue[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x68};
    const unsigned char first_connection[] = {0x48,0x8b,0x43,0x28,0x48,0x83,0xf8,0xff,0x74,0x3a};
    const unsigned char second_connection[] = {0x48,0x8b,0x43,0x38,0x48,0x83,0xf8,0xff,0x74,0x3a};
    // Increment the supplied counter, then undo the complete original prologue.
    const unsigned char destructor_body[] = {0x48,0xff,0x07,0x48,0x83,0xc4,0x68,0x5b,
        0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3};
    const unsigned char webapi[] = {0x4c,0x89,0xff,0xe8,0xa8,0x05,0,0,
        0x4c,0x89,0xff,0xe8,0x90,0xf9,0xff,0xff,0xc3};
    const unsigned char cleanup[] = {0xff,0x07,0xc3}; // inc dword [rdi]; ret
    const unsigned char retry[] = {0xff,0x47,0x04,0xb8,0x01,0x00,0x00,0x00,0xc3};
    unsigned char webapi_caller[] = {0x41,0x57,0x48,0x83,0xec,0x08,0x49,0x89,0xff,
        0xe8,0,0,0,0,0x48,0x83,0xc4,0x08,0x41,0x5f,0xc3};
    const int32_t call_displacement = static_cast<int32_t>(webapi_offset - (0x1000 + 14));
    std::memcpy(webapi_caller + 10, &call_displacement, sizeof(call_displacement));
    std::memcpy(image + predicate_offset, predicate, sizeof(predicate));
    std::memcpy(image + destructor_offset, prologue, sizeof(prologue));
    std::memcpy(image + destructor_offset + sizeof(prologue), destructor_body, sizeof(destructor_body));
    std::memcpy(image + 0xea24f3, first_connection, sizeof(first_connection));
    std::memcpy(image + 0xea2537, second_connection, sizeof(second_connection));
    std::memcpy(image + webapi_offset, webapi, sizeof(webapi));
    std::memcpy(image + 0x7d7420, cleanup, sizeof(cleanup));
    std::memcpy(image + 0x7d6810, retry, sizeof(retry));
    std::memcpy(image + 0x1000, webapi_caller, sizeof(webapi_caller));
    if (mprotect(image, image_size, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
    guest_cpu_code(runtime_image_base, image_size);
    auto* counter = reinterpret_cast<uint64_t*>(image + 0x10000);
    const uint64_t object = reinterpret_cast<uintptr_t>(counter);
    // Warm the original translation. The installer's invalidation is required
    // for the subsequent call to visit close_stream and its guest trampoline.
    guest_cpu_call(runtime_image_base + destructor_offset, 1, &object);
    if (!expect("unpatched synthetic stream destructor", *counter, 1)) return false;
    runtime_exit_initialize();
    if (!expect("stream shutdown hook installed", image[destructor_offset], 0x48)
            || !expect("WebAPI shutdown hook installed", image[webapi_offset + 3], 0x48)) return false;
    const uint64_t bridge_before = guest_bridge_calls;
    guest_cpu_call(runtime_image_base + destructor_offset, 1, &object);
    if (!expect("stream destructor preserves original body", *counter, 2)
            || !expect("stream shutdown re-enters guest CPU after invalidation",
                guest_bridge_calls - bridge_before, 2)) return false;
    struct RegionCase { uint32_t region, button; };
    for (const auto& item : {RegionCase{2,0x2000}, RegionCase{4,0x2000},
                            RegionCase{5,0x2000}, RegionCase{0x100,0x4000}}) {
        std::memcpy(image + region_offset, &item.region, sizeof(item.region));
        runtime_exit_tick();
        if (!expect("regional confirm predicate uses guest CPU and AL result",
                runtime_exit_confirm_button(), item.button)) return false;
    }
    auto* manager = image + 0x11000;
    const uint64_t manager_argument = reinterpret_cast<uintptr_t>(manager);
    auto* counts = reinterpret_cast<uint32_t*>(manager);
    if (!expect("WebAPI retry calls guest cleanup and initialization",
                guest_cpu_call(runtime_image_base + 0x1000, 1, &manager_argument) & 0xff, 1)
            || !expect("WebAPI cleanup count before stop", counts[0], 1)
            || !expect("WebAPI initialization count before stop", counts[1], 1)) return false;
    manager[0x21] = 1;
    if (!expect("WebAPI stop request leaves retry loop",
                guest_cpu_call(runtime_image_base + 0x1000, 1, &manager_argument) & 0xff, 1)
            || !expect("WebAPI cleanup retained on stop", counts[0], 2)
            || !expect("WebAPI initialization skipped on stop", counts[1], 1)) return false;
    guest_cpu_code_changed(runtime_image_base, image_size);
    runtime_image_base = runtime_image_size = 0;
    return munmap(image, image_size) == 0;
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
#ifdef DRIVECLUB_NATIVE_EXIT_TEST
    std::puts("Native x86-64 synthetic exit-hook test; ARM64 FEX is verified separately.");
    return test_exit_guest_bridges() ? 0 : 8;
#endif
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
    if (!test_boot_heaps(guest)) return 7;
    if (!test_exit_guest_bridges()) return 8;
    auto* context_memory = static_cast<unsigned char*>(runtime_low_map(16384,PROT_READ|PROT_WRITE|PROT_EXEC));
    if (!test_guest_setcontext_contract(context_memory,16384)) return 9;
    if (runtime_low_unmap(context_memory,16384)) return 9;
    guest_cpu_thread_end();
    std::puts("FEX passed the packaged Bionic ARM64 ABI, 39-bit heaps, PRT, controls, shutdown and libunwind regressions.");
    return 0;
}
