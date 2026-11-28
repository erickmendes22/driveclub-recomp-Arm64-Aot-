/* cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
 *   -I. tests/test_runtime_capture.c src/runtime_capture.c src/runtime.c \
 *   -Wl,--gc-sections -pthread -o out/test-runtime-capture */
#include "../src/runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>

static void test_recording(void) {
    typedef int32_t (ABI *SetInfo)(uint32_t,const void *,uint64_t);
    SetInfo set=(SetInfo)runtime_capture_resolve("Fc8qxlKINYQ#arbitrary#tags");
    assert(set);
    const int32_t invalid=(int32_t)UINT32_C(0x80a80003);
    const int32_t unsupported=(int32_t)UINT32_C(0x80a80008);
    struct { uint64_t before; uint32_t value; unsigned char after[12]; } input;
    memset(&input,0xa5,sizeof(input)); input.value=0;
    unsigned char original[sizeof(input)]; memcpy(original,&input,sizeof(input));
    assert(set(0xa007,&input.value,4)==unsupported); /* Actual initializer. */
    assert(set(0xd,&input.value,4)==unsupported);    /* ChapterChange. */
    input.value=1;
    assert(set(0xd,&input.value,4)==unsupported);    /* ChapterProhibit. */
    input.value=0;
    assert(!memcmp(original,&input,sizeof(input)));
    const uint32_t known[]={0x2,0x6,0x7,0x8,0xd,0xa01,0xa007,0xa008,0xa009};
    for (unsigned i=0;i<sizeof(known)/sizeof(*known);++i) {
        assert(set(known[i],NULL,4)==invalid);
        assert(set(known[i],&input.value,UINT64_MAX)==unsupported);
    }
    assert(set(0,&input.value,4)==invalid && set(0xa00a,&input.value,4)==invalid);
    assert(set(UINT32_MAX,&input.value,4)==invalid);
    assert(!memcmp(original,&input,sizeof(input)));
    assert(!runtime_capture_resolve("unknown####"));
    puts("PASS: recording ABI/NID, observed boot requests, unsupported service, invalid metadata and unchanged input buffers");
}

typedef int32_t (ABI *Toggle)(void);
static Toggle enable_capture,disable_capture;
static int applied,policy_calls,policy_order[16],fail_policy,reenter_policy,retire_policy;
static int32_t nested_result;
static _Atomic int block_policy,policy_entered,release_policy;
static int apply_policy(int enabled) {
    assert(policy_calls<16);
    policy_order[policy_calls++]=enabled;
    if (reenter_policy) nested_result=disable_capture();
    if (retire_policy) runtime_capture_set_screenshot_hook(NULL);
    if (atomic_load(&block_policy)) {
        atomic_store(&policy_entered,1);
        while (!atomic_load(&release_policy)) sched_yield();
    }
    if (fail_policy) return -1;
    applied=enabled;
    return 0;
}
static void *change_from_worker(void *result) {
    *(int32_t *)result=enable_capture();
    return NULL;
}
static void test_screenshot_policy(void) {
    const int32_t unavailable=(int32_t)UINT32_C(0x8002004e);
    const int32_t io_error=(int32_t)UINT32_C(0x80020005);
    const int32_t busy=(int32_t)UINT32_C(0x80020010);
    enable_capture=(Toggle)runtime_capture_resolve("2xxUtuC-RzE#anything#tags");
    disable_capture=(Toggle)runtime_capture_resolve("tIYf0W5VTi8#anything#tags");
    assert(enable_capture && disable_capture);
    assert(enable_capture()==unavailable && disable_capture()==unavailable);
    assert(!policy_calls && !applied);
    runtime_capture_set_screenshot_hook(apply_policy);
    assert(!enable_capture() && applied==1);
    assert(!disable_capture() && applied==0);
    assert(policy_calls==2 && policy_order[0]==1 && policy_order[1]==0);
    fail_policy=1;
    assert(enable_capture()==io_error && applied==0);
    fail_policy=0; reenter_policy=1;
    assert(!enable_capture() && applied==1 && nested_result==busy);
    assert(policy_calls==4); /* The reentrant call never reached the backend. */
    reenter_policy=0;

    block_policy=1;
    pthread_t worker;
    int32_t result=123;
    assert(!pthread_create(&worker,NULL,change_from_worker,&result));
    while (!atomic_load(&policy_entered)) sched_yield();
    assert(disable_capture()==busy);
    atomic_store(&release_policy,1);
    assert(!pthread_join(worker,NULL) && result==0 && applied==1);
    block_policy=0;

    /* Even if an old backend reports success, removing it during the request
     * must not claim that the current window policy was applied. */
    retire_policy=1;
    assert(disable_capture()==io_error);
    retire_policy=0;
    assert(enable_capture()==unavailable);
    runtime_capture_set_screenshot_hook(apply_policy);
    assert(!enable_capture() && applied==1);
    runtime_capture_set_screenshot_hook(NULL);
    puts("PASS: screenshot hook ordering, absence/failure propagation, reentrancy and concurrent exclusion without a callback lock");
}

int main(void) {
    alarm(10);
    test_recording();
    test_screenshot_policy();
    alarm(0);
    return 0;
}
