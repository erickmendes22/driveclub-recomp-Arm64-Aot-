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

static size_t low_mapping_size(size_t size) {
    size_t alignment=GUEST_PAGE;
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (alignment<info.dwAllocationGranularity) alignment=info.dwAllocationGranularity;
#else
    long host_page=sysconf(_SC_PAGESIZE);
    if (host_page<=0 || (host_page & (host_page-1))) { errno=EINVAL; return 0; }
    if (alignment<(size_t)host_page) alignment=(size_t)host_page;
#endif
    if (!size) { errno=EINVAL; return 0; }
    if (size>LOW_LIMIT-LOW_MIN-(alignment-1)) { errno=ENOMEM; return 0; }
    return (size+alignment-1)&~(alignment-1);
}

void *runtime_low_map(size_t size, int prot) {
    size=low_mapping_size(size);
    if (!size) return NULL;
    /* low_mapping_size is a multiple of the host and guest granularity. */
    size_t gap=low_mapping_size(1);
    void *result=NULL;
    pthread_mutex_lock(&low_lock);
    while (low_next<LOW_LIMIT && size<=LOW_LIMIT-low_next) {
        uintptr_t address=low_next;
        low_next+=size+gap; /* An unmapped gap catches overruns. */
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

int runtime_low_unmap(void *address, size_t size) {
    size=low_mapping_size(size);
    uintptr_t start=(uintptr_t)address;
    if (!size) return -1;
    if (start<LOW_MIN || start>=LOW_LIMIT || size>LOW_LIMIT-start) { errno=EINVAL; return -1; }
#ifdef _WIN32
    if (VirtualFree(address,0,MEM_RELEASE)) return 0;
    errno=EINVAL;
    return -1;
#else
    return munmap(address,size);
#endif
}
