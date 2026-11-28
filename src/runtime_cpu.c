/* guest_cpu.h on x86-64 hosts: the game's code runs on the host CPU, so crossings are plain calls.
 * Other hosts implement guest_cpu.h in libbbcpu.so (cpu/guest_cpu_fex.cpp, FEXCore). */
#define _GNU_SOURCE
#include "guest_cpu.h"
#if defined(GUEST_CPU_NATIVE)
#include "runtime.h"
#include "guest_setcontext_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>
#include <pthread.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#endif

void guest_cpu_init(void *(*map)(size_t size, int prot)) { (void)map; }
void guest_cpu_code(uintptr_t base, size_t size) { (void)base; (void)size; }
void guest_cpu_code_changed(uintptr_t address, size_t size) { (void)address; (void)size; }
static ABI __attribute__((noreturn)) void invalid_libunwind_context(const void *context,uint64_t reason) {
    fprintf(stderr,"STOP: invalid __Ux86_64_setcontext at %p (reason %llu: 1 pointer/alignment, 2 mc_len, 3 RIP/RSP, 4 FP owner, 5 FP format)\n",
            context,(unsigned long long)reason);
    fflush(stderr);
    _exit(21);
}
int guest_cpu_libunwind_setcontext(const void *context) {
    invalid_libunwind_context(context,0); /* Must be bound through guest_cpu_host_function. */
}
static pthread_once_t restore_once=PTHREAD_ONCE_INIT;
static uintptr_t restore_entry;
static void create_restore_entry(void) {
    unsigned char *code=runtime_low_map(sizeof(guest_setcontext_code),PROT_READ|PROT_WRITE);
    if (!code) { fputs("STOP: cannot allocate libunwind restore entry\n",stderr); _exit(21); }
    memcpy(code,guest_setcontext_code,sizeof(guest_setcontext_code));
    uintptr_t failure=(uintptr_t)invalid_libunwind_context;
    memcpy(code+GUEST_SETCONTEXT_FAILURE_OFFSET,&failure,sizeof(failure));
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect(code,sizeof(guest_setcontext_code),PAGE_EXECUTE_READ,&old)) {
#else
    if (mprotect(code,(sizeof(guest_setcontext_code)+16383)&~(size_t)16383,PROT_READ|PROT_EXEC)) {
#endif
        fputs("STOP: cannot protect libunwind restore entry\n",stderr); _exit(21);
    }
    restore_entry=(uintptr_t)code;
}
uintptr_t guest_cpu_host_function(void *fn) {
    if (fn==(void *)guest_cpu_libunwind_setcontext) {
        pthread_once(&restore_once,create_restore_entry);
        return restore_entry;
    }
    return (uintptr_t)fn;
}
uint64_t guest_cpu_call(uintptr_t fn, unsigned count, const uint64_t *args) {
    typedef uint64_t (ABI *Fn)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
    uint64_t a[6]={0};
    if (count>6) { fputs("STOP: guest_cpu_call with more than 6 arguments\n",stderr); exit(21); }
    for (unsigned i=0;i<count;++i) a[i]=args[i];
    return ((Fn)fn)(a[0],a[1],a[2],a[3],a[4],a[5]);
}
uint64_t guest_cpu_call_xmm(uintptr_t fn, unsigned count, const uint64_t *args, unsigned xmm_count, const uint64_t *xmm) {
    (void)fn; (void)count; (void)args; (void)xmm_count; (void)xmm;
    fputs("STOP: guest_cpu_call_xmm on an x86-64 host (call the function directly)\n",stderr); exit(21);
}
void guest_cpu_thread_stack(void *top, size_t size) { (void)top; (void)size; }
void guest_cpu_abandon(void) {}
void guest_cpu_thread_end(void) {}
uintptr_t guest_cpu_return_address(uintptr_t native) { return native; }
uintptr_t guest_cpu_stack_pointer(void) { return (uintptr_t)__builtin_frame_address(0); }
int guest_cpu_hook(uintptr_t address, size_t size, GuestHook hook) { (void)address; (void)size; (void)hook; return 0; }
uint64_t guest_cpu_tsc(void) { return __rdtsc(); }
#ifndef _WIN32
uint64_t guest_cpu_tsc_frequency(void) {
    static uint64_t hz;
    if (!hz) {
        struct timespec a,b,nap={0,20000000};
        clock_gettime(CLOCK_MONOTONIC,&a); uint64_t t0=__rdtsc();
        nanosleep(&nap,NULL);
        clock_gettime(CLOCK_MONOTONIC,&b); uint64_t t1=__rdtsc();
        uint64_t ns=(uint64_t)(b.tv_sec-a.tv_sec)*1000000000+(uint64_t)(b.tv_nsec-a.tv_nsec);
        hz=(t1-t0)*1000000000/(ns ? ns : 1);
    }
    return hz;
}
void guest_cpu_set_gs(void *base) {
    if (syscall(SYS_arch_prctl,ARCH_SET_GS,(unsigned long)base)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); exit(21); }
}
int guest_cpu_signal_regs(const void *ucontext, GuestRegs *regs) {
    static const int order[GUEST_GPRS]={REG_RAX,REG_RCX,REG_RDX,REG_RBX,REG_RSP,REG_RBP,REG_RSI,REG_RDI,
                                        REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15};
    const greg_t *g=((const ucontext_t *)ucontext)->uc_mcontext.gregs;
    for (int i=0;i<GUEST_GPRS;++i) regs->gpr[i]=(uint64_t)g[order[i]];
    regs->rip=(uint64_t)g[REG_RIP];
    return GUEST_CPU_IN_GUEST;
}
uintptr_t guest_cpu_host_pc(const void *ucontext) { return (uintptr_t)((const ucontext_t *)ucontext)->uc_mcontext.gregs[REG_RIP]; }
int guest_cpu_handle_fault(int sig, siginfo_t *info, void *ucontext) { (void)sig; (void)info; (void)ucontext; return 0; }
#endif
#else
typedef int runtime_cpu_unused; /* guest_cpu.h is implemented by libbbcpu.so (cpu/) */
#endif
