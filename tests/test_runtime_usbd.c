/* Real resolver/empty-USB lifecycle; no host USB devices or game files used. */
#define _GNU_SOURCE
#include "../src/runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

typedef struct { int64_t sec, usec; } Timeval;
typedef int32_t (ABI *Init)(void);
typedef void (ABI *Exit)(void);
typedef int64_t (ABI *List)(void ***);
typedef void (ABI *FreeList)(void **,int32_t);
typedef int32_t (ABI *Events)(const Timeval *);
static Events events;
static double seconds(void) {
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC,&now));
    return (double)now.tv_sec+(double)now.tv_nsec/1e9;
}
static void *wait_events(void *unused) {
    (void)unused;
    const int32_t result=events(&(Timeval){5,0});
    // If scheduling delays this thread until after Exit, it must reject the
    // closed context promptly instead of waiting on an uninitialized backend.
    assert(result==0 || result==(int32_t)0x802400ff);
    return NULL;
}
int main(void) {
    Init init=(Init)runtime_usbd_resolve("TOhg7P6kTH4#u#v");
    Exit end=(Exit)runtime_usbd_resolve("Fq6+0Fm55xU#u#v");
    List list=(List)runtime_usbd_resolve("8qB9Ar4P5nc#u#v");
    FreeList free_list=(FreeList)runtime_usbd_resolve("EQ6SCLMqzkM#u#v");
    events=(Events)runtime_usbd_resolve("+wU6CGuZcWk#u#v");
    assert(init && end && list && free_list && events);
    void **a=(void **)1, **b=NULL;
    assert(list(&a)==(int32_t)0x802400ff && !a);
    assert(!init() && !init());
    assert(list(NULL)==(int32_t)0x80240002);
    assert(!list(&a) && !list(&b) && a && b && a!=b && !a[0] && !b[0]);
    assert((uintptr_t)a<UINT64_C(1)<<40 && (uintptr_t)b<UINT64_C(1)<<40);
    free_list(a,1); free_list(a,1); free_list(NULL,0);
    end(); /* A second client still owns the initialized context. */
    assert(!events(&(Timeval){0,0}));
    assert(events(&(Timeval){0,1000000})==(int32_t)0x80240002);
    assert(events(&(Timeval){-1,0})==(int32_t)0x80240002);
    double begin=seconds();
    assert(!events(&(Timeval){0,25000}));
    assert(seconds()-begin>=0.020); /* The game's USB event thread must not spin. */
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,wait_events,NULL));
    struct timespec nap={0,30000000};
    nanosleep(&nap,NULL);
    begin=seconds();
    end();
    assert(!pthread_join(thread,NULL));
    assert(seconds()-begin<1.0); /* Closing wakes the waiter before its 5 s timeout. */
    free_list(b,1); /* Lists remain owned by the caller after Exit. */
    assert(events(&(Timeval){0,0})==(int32_t)0x802400ff);
    assert(!init() && !list(&a) && a && !a[0]);
    free_list(a,0); end();
    assert(!runtime_usbd_resolve("0ktE1PhzGFU#u#v")); /* No fake transfer provider. */
    puts("PASS: USB empty enumeration, low pointers, refcounts, real timeout and shutdown wakeup");
    return 0;
}
