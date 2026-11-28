/* Focused loader-state test. The constructor callback is synthetic; the
 * production LinkedModule state machine and kernel API adapters are included.
 * cc -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -I. -Isrc \
 *   tests/test_runtime_modules.c -Wl,--gc-sections -pthread -lm -o out/test-runtime-modules */
#define _GNU_SOURCE
#include <assert.h>
#include <stdatomic.h>
#define main unused_probe_main
#include "../src/probe.c"
#undef main
#include "../src/runtime_kernel.c"

static _Atomic unsigned constructor_calls;
static _Atomic int constructor_entered,constructor_release;
static int32_t constructor_result;
static int constructor_blocks,constructor_reenters;
static const uint64_t argument_size=8;
static const uint64_t argument_value=UINT64_C(0x123456789abcdef0);

uint64_t guest_cpu_call(uintptr_t entry,unsigned count,const uint64_t *args) {
    assert(entry==(uintptr_t)image+modules[1].init);
    assert(count==3 && args[0]==argument_size && args[1]==(uintptr_t)&argument_value && args[2]==0);
    atomic_fetch_add(&constructor_calls,1);
    if (constructor_reenters) {
        int32_t result=123;
        assert(runtime_start_linked_module("/app0/oskGNMm.prx",0,NULL,&result)==ERR(16));
        assert(result==123);
    }
    atomic_store(&constructor_entered,1);
    while (constructor_blocks && !atomic_load(&constructor_release)) sched_yield();
    return (uint64_t)(int64_t)constructor_result;
}

static void reset_modules(void) {
    static unsigned char synthetic_image[64];
    image=synthetic_image;
    memset(modules,0,sizeof(modules));
    module_count=2;
    snprintf(modules[0].name,sizeof(modules[0].name),"libtbb.prx");
    modules[0].started=1;
    snprintf(modules[1].name,sizeof(modules[1].name),"oskGNMm.prx");
    modules[1].flags=MODULE_START_ON_LOAD;
    modules[1].init=16;
    constructor_calls=constructor_entered=constructor_release=0;
    constructor_result=constructor_blocks=constructor_reenters=0;
}

static void test_stable_handles_and_pinning(void) {
    reset_modules();
    assert(runtime_linked_module_handle("/app0/libtbb.prx")==0x1000);
    assert(runtime_linked_module_handle("/app0/oskGNMm.prx")==0x1001);
    assert(runtime_linked_module_handle("unknown.prx")==ERR(2));
    assert(runtime_linked_module_handle(NULL)==ERR(2));
    int32_t result=123;
    assert(kernel_load_start_module("/app0/libtbb.prx",0,NULL,0,NULL,&result)==0x1000 && result==0);
    assert(kernel_load_start_module("/app0/libtbb.prx",0,NULL,0,NULL,&result)==0x1000);
    assert(kernel_load_start_module("missing.prx",0,NULL,0,NULL,&result)==ERR(2));
    assert(!atomic_load(&constructor_calls));
    result=456;
    assert(kernel_stop_unload_module(0x1000,0,NULL,0,NULL,&result)==ERR(16) && result==456);
    assert(kernel_stop_unload_module(0x1002,0,NULL,0,NULL,&result)==ERR(3) && result==456);
    assert(kernel_stop_unload_module(0x1000,0,NULL,1,NULL,&result)==ERR(22));
    assert(kernel_load_start_module(NULL,0,NULL,0,NULL,&result)==ERR(14));
    assert(kernel_load_start_module("libtbb.prx",4,NULL,0,NULL,&result)==ERR(22));
    puts("PASS: known modules have stable handles; unknown paths fail; pinned unload returns EBUSY");
}

static void test_failed_start_cached(void) {
    reset_modules();
    constructor_result=-37;
    int32_t result=0;
    assert(kernel_load_start_module("oskGNMm.prx",argument_size,&argument_value,0,NULL,&result)==ERR(100));
    assert(result==-37 && !modules[1].started && modules[1].start_failed);
    result=0;
    assert(kernel_load_start_module("oskGNMm.prx",argument_size,&argument_value,0,NULL,&result)==ERR(100));
    assert(result==-37 && constructor_calls==1);
    puts("PASS: failed module initializer remains failed with its original result and is not rerun");
}

static void *start_module(void *opaque) {
    int32_t *output=opaque;
    int32_t result=123;
    *output=kernel_load_start_module("oskGNMm.prx",argument_size,&argument_value,0,NULL,&result);
    assert(result==0);
    return NULL;
}
static void test_concurrent_start(void) {
    reset_modules();
    constructor_blocks=constructor_reenters=1;
    pthread_t thread;
    int32_t started=0;
    assert(!pthread_create(&thread,NULL,start_module,&started));
    while (!atomic_load(&constructor_entered)) sched_yield();
    int32_t result=123;
    assert(kernel_load_start_module("oskGNMm.prx",argument_size,&argument_value,0,NULL,&result)==ERR(16));
    assert(result==123);
    atomic_store(&constructor_release,1);
    assert(!pthread_join(thread,NULL));
    assert(started==0x1001 && constructor_calls==1 && modules[1].started && !modules[1].starting);
    assert(kernel_load_start_module("oskGNMm.prx",argument_size,&argument_value,0,NULL,&result)==0x1001);
    assert(result==0 && constructor_calls==1);
    puts("PASS: concurrent/reentrant start reports EBUSY, without a loader lock across guest constructors");
}

int main(void) {
    alarm(15);
    test_stable_handles_and_pinning();
    test_failed_start_cached();
    test_concurrent_start();
    alarm(0);
    return 0;
}
