/* cc -std=c11 -O2 -Wall -Wextra -I. -Isrc tests/test_guest_setcontext.c \
 *    src/runtime_cpu.c src/runtime_low_memory.c -pthread -o out/test-guest-setcontext */
#define _GNU_SOURCE
#include "../src/runtime.h"
#include "guest_setcontext_contract.h"
#include <assert.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void invalid_context(unsigned char *context,unsigned kind) {
    pid_t child=fork();
    assert(child>=0);
    if (!child) {
        memset(context,0,0x500);
        context_put64(context,0x108,0x480);
        context_put64(context,0xe0,1);
        context_put64(context,0xf8,16);
        context_put64(context,0x110,0x10002);
        context_put64(context,0x118,0x20001);
        switch (kind) {
        case 1: ++context; break;
        case 2: context_put64(context,0x108,0); break;
        case 3: context_put64(context,0xe0,0); break;
        case 4: context_put64(context,0x118,0x40000); break;
        case 5: context_put64(context,0x110,0x12345); break;
        default: assert(0);
        }
        uintptr_t entry=guest_cpu_host_function((void *)guest_cpu_libunwind_setcontext);
        uint64_t args[]={(uintptr_t)context};
        guest_cpu_call(entry,1,args);
        _exit(99);
    }
    int status;
    assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==21);
}

int main(void) {
    guest_cpu_init(runtime_low_map);
    unsigned char *memory=runtime_low_map(16384,PROT_READ|PROT_WRITE|PROT_EXEC);
    assert(memory);
    assert(test_guest_setcontext_contract(memory,16384));
    fflush(NULL);
    for (unsigned kind=1;kind<=5;++kind) invalid_context(memory+0x400,kind);
    puts("PASS: invalid libunwind pointer, length, control flow and FP metadata stop explicitly");
    assert(runtime_low_unmap(memory,16384)==0);
    return 0;
}
