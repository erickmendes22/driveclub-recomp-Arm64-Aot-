/* Shared native-x86 / packaged-FEX contract, using only synthetic guest code.
 * The caller supplies at least 4096 writable/executable bytes in the low arena. */
#ifndef DRIVECLUB_GUEST_SETCONTEXT_CONTRACT_H
#define DRIVECLUB_GUEST_SETCONTEXT_CONTRACT_H
#include "../src/guest_cpu.h"
#include "guest_setcontext_fixture.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void context_put64(unsigned char *p,size_t offset,uint64_t value) {
    memcpy(p+offset,&value,sizeof(value));
}
static uint64_t context_get64(const unsigned char *p,size_t offset) {
    uint64_t value;
    memcpy(&value,p+offset,sizeof(value));
    return value;
}
static int test_guest_setcontext_contract(unsigned char *memory,size_t size) {
    if (!memory || size<4096 || ((uintptr_t)memory&15)) return 0;
    unsigned char *context=memory+0x400,*observed=memory+0xa00;
    static const unsigned offsets[]={0x78,0x80,0x88,0xa0,0xa8,0xb0,0xb8,
                                      0x48,0x50,0x58,0x60,0x68,0x70};
    static const char *const registers[]={"RAX","RBX","RBP","R12","R13","R14","R15",
                                          "RDI","RSI","RDX","RCX","R8","R9"};
    const uint64_t pattern=UINT64_C(0x1234567800000100);
    memset(memory,0,4096);
    memcpy(memory,guest_setcontext_fixture,sizeof(guest_setcontext_fixture));
    for (unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);++i) context_put64(context,offsets[i],pattern+i);
    context_put64(context,0x108,0x480);
    context_put64(context,0x110,0x10002);
    context_put64(context,0x118,0x20001);
    const uintptr_t restore=guest_cpu_host_function((void *)guest_cpu_libunwind_setcontext);
    guest_cpu_code((uintptr_t)memory,4096);
    guest_cpu_code_changed((uintptr_t)memory,sizeof(guest_setcontext_fixture));
    const uint64_t args[]={(uintptr_t)context,(uintptr_t)observed,restore};
    if (guest_cpu_call((uintptr_t)memory,3,args)!=42) {
        fputs("FAIL: libunwind restore did not return through the saved continuation\n",stderr);
        return 0;
    }
    for (unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);++i) {
        if (context_get64(observed,8*i)!=pattern+i) {
            fprintf(stderr,"FAIL: libunwind restore %s mismatch\n",registers[i]);
            return 0;
        }
    }
    puts("PASS: libunwind restores RAX, argument and callee-saved registers");
    if (context_get64(observed,0xf0)!=1
            || context_get64(observed,0x68)!=context_get64(context,0xf8)
            || context_get64(observed,0x70)!=context_get64(context,0xf8)
            || context_get64(observed,0x78)!=context_get64(context,0xe0)) {
        fputs("FAIL: libunwind restore RIP/RSP continuation mismatch\n",stderr);
        return 0;
    }
    puts("PASS: libunwind transfers to saved RIP/RSP without returning to its caller");
    uint32_t mxcsr;
    memcpy(&mxcsr,observed+0x118,sizeof(mxcsr));
    static const unsigned char st0_one[]={0,0,0,0,0,0,0,0x80,0xff,0x3f};
    if (mxcsr!=0x5f80 || memcmp(observed+0x120,st0_one,sizeof(st0_one))
            || memcmp(observed+0x1a0,context+0x1c0,16)
            || memcmp(observed+0x210,context+0x230,16)) {
        fputs("FAIL: libunwind x87/SSE/MXCSR restore mismatch\n",stderr);
        return 0;
    }
    puts("PASS: libunwind restores x87, SSE vectors and MXCSR");
    return 1;
}
#endif
