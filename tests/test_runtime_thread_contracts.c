/* Real pthread/TLS contracts. Include the implementation to test the narrow
 * entrypoints without pulling unrelated GPU/service exports into the binary.
 * Only diagnostics are inert; threads, memory maps and destructors are real.
 * cc -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -I. -Isrc \
 *   tests/test_runtime_thread_contracts.c src/runtime_cpu.c src/runtime_low_memory.c \
 *   -Wl,--gc-sections -Wl,--wrap=nanosleep -pthread -o out/test-runtime-threads */
#define _GNU_SOURCE
#include <assert.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define handlers process_exit_handlers
#include "../src/runtime.c"
#undef handlers
#include "../src/runtime_kernel.c"
#include "../src/runtime_thread.c"

void runtime_diag_name(const char *name) { (void)name; }
void runtime_diag_wait_begin(const char *kind,uint64_t value,const void *caller) {
    (void)kind; (void)value; (void)caller;
}
void runtime_diag_wait_end(void) {}

int __real_nanosleep(const struct timespec *,struct timespec *);
static int sleep_mode,sleep_calls;
static struct timespec sleep_seen[3];
int __wrap_nanosleep(const struct timespec *request,struct timespec *remaining) {
    if (!sleep_mode) return __real_nanosleep(request,remaining);
    assert(sleep_calls<3);
    sleep_seen[sleep_calls++]=*request;
    if (sleep_mode==2 && sleep_calls==1) {
        *remaining=(struct timespec){1,250000000}; errno=EINTR; return -1;
    }
    if (sleep_mode==3) { errno=EIO; return -1; }
    return 0;
}

static void test_seconds_sleep(void) {
    assert(kernel_sleep(0)==0);
    sleep_mode=1; sleep_calls=0;
    assert(kernel_sleep(UINT32_MAX)==0);
    assert(sleep_calls==1 && sleep_seen[0].tv_sec==UINT32_MAX && !sleep_seen[0].tv_nsec);
    sleep_mode=2; sleep_calls=0;
    assert(kernel_sleep(2)==0);
    assert(sleep_calls==2 && sleep_seen[0].tv_sec==2 && sleep_seen[1].tv_sec==1
            && sleep_seen[1].tv_nsec==250000000);
    sleep_mode=3; sleep_calls=0;
    assert(kernel_sleep(1)==ERR(5));
    sleep_mode=0;
    puts("PASS: seconds sleep preserves its full range, resumes host EINTR and reports host failure");
}

typedef struct {
    GuestThread *handle;
    _Atomic int ready, release;
    unsigned char *tls, *other_tls;
    int self_detach, detach_result, destructor_calls;
} Work;
static uint32_t work_key;
static const unsigned char template2[]={1,2,3,4},template3[]={7,8};

static void ABI work_destructor(void *opaque) {
    Work *w=opaque;
    const uint64_t index[]={2,3};
    assert(guest_tls_get_addr(index)==w->tls+3 && w->tls[3]==0x4a);
    assert(w->other_tls[1]==0x6b);
    if (++w->destructor_calls<2) assert(key_set(work_key,w)==0);
}
static void *ABI worker(void *opaque) {
    Work *w=opaque;
    const uint64_t index2[]={2,0},index3[]={3,0};
    w->tls=guest_tls_get_addr(index2);
    w->other_tls=guest_tls_get_addr(index3);
    assert((uintptr_t)w->tls<UINT64_C(0x10000000000)
            && (uintptr_t)w->other_tls<UINT64_C(0x10000000000));
    assert(!memcmp(w->tls,template2,sizeof(template2)) && !memcmp(w->other_tls,template3,sizeof(template3)));
    for (unsigned i=sizeof(template2);i<19;++i) assert(w->tls[i]==0);
    w->tls[3]=0x4a; w->other_tls[1]=0x6b;
    assert(key_set(work_key,w)==0);
    if (w->self_detach) w->detach_result=posix_detach(thread_self());
    atomic_store(&w->ready,1);
    while (!atomic_load(&w->release)) sched_yield();
    return (void *)(uintptr_t)0x55aa;
}
static void start_work(Work *w,ThreadAttr **attr) {
    assert(thread_create(&w->handle,attr,worker,w,"contract")==0);
    while (!atomic_load(&w->ready)) sched_yield();
}
static void finish_work(Work *w) {
    atomic_store(&w->release,1);
    for (;;) {
        pthread_mutex_lock(&lock);
        int done=w->handle->finished;
        pthread_mutex_unlock(&lock);
        if (done) break;
        sched_yield();
    }
    unsigned char residency;
    assert(w->destructor_calls==2);
    errno=0;
    assert(mincore(w->tls,4096,&residency)==-1 && errno==ENOMEM);
    errno=0;
    assert(mincore(w->other_tls,4096,&residency)==-1 && errno==ENOMEM);
}

typedef struct { Work *work; pthread_barrier_t *barrier; int detach,result; } Race;
static void *race_operation(void *opaque) {
    Race *r=opaque;
    pthread_barrier_wait(r->barrier);
    r->result=r->detach ? posix_detach(r->work->handle) : posix_join(r->work->handle,NULL);
    return NULL;
}
static void test_thread_detach_and_tls(void) {
    runtime_set_module_tls(2,template2,sizeof(template2),19);
    runtime_set_module_tls(3,template3,sizeof(template3),33);
    assert(key_create(&work_key,work_destructor)==0);
    *runtime_errno()=123;
    assert(posix_detach((void *)(uintptr_t)1)==3 && *runtime_errno()==123);
    assert(posix_detach(thread_self())==22);
    const uint64_t index[]={2,0};
    unsigned char *main_tls=guest_tls_get_addr(index);
    Work joinable={0}; start_work(&joinable,NULL);
    assert(joinable.tls!=main_tls);
    atomic_store(&joinable.release,1);
    void *result=NULL;
    assert(posix_join(joinable.handle,&result)==0 && result==(void *)(uintptr_t)0x55aa);
    assert(posix_join(joinable.handle,NULL)==22 && posix_detach(joinable.handle)==22);
    finish_work(&joinable);

    Work detached={0}; start_work(&detached,NULL);
    assert(posix_detach(detached.handle)==0);
    assert(posix_detach(detached.handle)==22 && posix_join(detached.handle,NULL)==22);
    /* The actual host resource is detached, not just a guest bookkeeping bit. */
    assert(pthread_join(detached.handle->host,NULL)==EINVAL);
    finish_work(&detached);

    Work self={.self_detach=1}; start_work(&self,NULL);
    assert(self.detach_result==0 && posix_join(self.handle,NULL)==22);
    finish_work(&self);
    ThreadAttr *attr=NULL;
    assert(attr_init(&attr)==0 && attr_set_detach(&attr,1)==0);
    Work from_attr={.self_detach=1}; start_work(&from_attr,&attr);
    assert(from_attr.detach_result==22 && posix_join(from_attr.handle,NULL)==22);
    finish_work(&from_attr);
    assert(attr_destroy(&attr)==0);

    for (unsigned i=0;i<8;++i) {
        Work work={0}; start_work(&work,NULL);
        pthread_barrier_t barrier;
        assert(!pthread_barrier_init(&barrier,NULL,3));
        Race a={.work=&work,.barrier=&barrier,.detach=0},b={.work=&work,.barrier=&barrier,.detach=1};
        pthread_t ta,tb;
        assert(!pthread_create(&ta,NULL,race_operation,&a));
        assert(!pthread_create(&tb,NULL,race_operation,&b));
        pthread_barrier_wait(&barrier);
        atomic_store(&work.release,1);
        assert(!pthread_join(ta,NULL) && !pthread_join(tb,NULL));
        assert((a.result==0 && b.result==22) || (a.result==22 && b.result==0));
        finish_work(&work);
        assert(!pthread_barrier_destroy(&barrier));
    }
    assert(key_delete(work_key)==0);
    uint32_t reused;
    assert(key_create(&reused,NULL)==0);
    assert(key_set(reused,(void *)(uintptr_t)123)==0);
    assert(key_delete(reused)==0);
    uint32_t newer;
    assert(key_create(&newer,NULL)==0 && newer==reused && key_get(newer)==NULL);
    assert(key_delete(newer)==0);
    assert(!memcmp(main_tls,template2,sizeof(template2)));
    runtime_thread_cleanup();
    unsigned char residency;
    assert(mincore(main_tls,4096,&residency)==-1 && errno==ENOMEM);
    puts("PASS: real detach, join exclusion, concurrent operations and detached creation");
    puts("PASS: low-address module TLS isolation, destructor ordering, key reuse and mapping release");
}

int main(void) {
    alarm(15);
    guest_cpu_init(runtime_low_map);
    test_seconds_sleep();
    test_thread_detach_and_tls();
    alarm(0);
    return 0;
}
