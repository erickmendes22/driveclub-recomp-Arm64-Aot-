/* Host-owned memory visible to the guest (image, stacks, trampolines).
 * Keep it below the PS4 VMA range and below 1 TiB: the game packs pointers
 * into 40-bit fields. This allocator deliberately has no loader/GPU dependencies. */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define LOW_MIN UINT64_C(0x0800000000)
#define LOW_LIMIT UINT64_C(0x1000000000)
#define GUEST_PAGE UINT64_C(16384)
static uintptr_t low_next=LOW_MIN;
static pthread_mutex_t low_lock=PTHREAD_MUTEX_INITIALIZER;

#ifdef _WIN32
static DWORD low_protection(int prot) {
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}
#endif

void *runtime_low_map(size_t size, int prot) {
    size_t alignment=GUEST_PAGE;
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (alignment<info.dwAllocationGranularity) alignment=info.dwAllocationGranularity;
#else
    long host_page=sysconf(_SC_PAGESIZE);
    if (host_page<=0 || (host_page & (host_page-1))) { errno=EINVAL; return NULL; }
    if (alignment<(size_t)host_page) alignment=(size_t)host_page;
#endif
    if (!size) { errno=EINVAL; return NULL; }
    if (size>LOW_LIMIT-LOW_MIN-(alignment-1)) { errno=ENOMEM; return NULL; }
    size=(size+alignment-1)&~(alignment-1);
    void *result=NULL;
    pthread_mutex_lock(&low_lock);
    while (low_next<LOW_LIMIT && size<=LOW_LIMIT-low_next) {
        uintptr_t address=low_next;
        low_next+=size+alignment; /* An unmapped gap catches overruns. */
#ifdef _WIN32
        result=VirtualAlloc((void *)address,size,MEM_RESERVE|MEM_COMMIT,low_protection(prot));
        if (result) break;
#else
        void *mapped=mmap((void *)address,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        if (mapped==MAP_FAILED) continue;
        /* Older kernels may ignore MAP_FIXED_NOREPLACE and treat the address as
         * a hint. Never return a high pointer or silently overwrite a mapping. */
        if ((uintptr_t)mapped==address) { result=mapped; break; }
        munmap(mapped,size);
#endif
    }
    pthread_mutex_unlock(&low_lock);
    if (!result) errno=ENOMEM;
    return result;
}
