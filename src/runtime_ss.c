/* inFAMOUS Second Son: the system calls bbport never needed (Bloodborne does not import them).
 *
 * Signatures and results follow the console's contract (checked against shadPS4's HLE and, for
 * libkernel internals, AnyPS5's). Where the port is single-player and offline, the answer is the
 * honest one for that situation: no entitlements, no dialogs open, nothing left to install. Each
 * such answer is logged once, so it shows in the log instead of looking like a real feature. */
#include "runtime.h"
#include "runtime_dialogs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#define gmtime_r(time, out) (gmtime_s((out), (time)) ? NULL : (out))
#endif

#define SCE_ERR(errno_) ((int32_t)(0x80020000u | (unsigned)(errno_)))
enum { E_NOENT = 2, E_SRCH = 3, E_FAULT = 14, E_INVAL = 22, E_NODEV = 19 };

static void log_once(volatile int *flag, const char *what) {
    if (__sync_lock_test_and_set(flag, 1)) return;
    printf("Runtime: %s\n", what);
}
#define ONCE(msg) do { static volatile int once_; log_once(&once_, msg); } while (0)

/* ---- libkernel: process and module information ------------------------------------ */
/* Base PS4 mode. The Pro-mode checkerboard path is a separate piece of work. */
static ABI int32_t kernel_is_neo(void) { return 0; }
static ABI int32_t kernel_mlock(const void *addr, uint64_t length) { (void)addr; (void)length; return 0; }
/* The unwinder asks which module owns an address; the linked image is one flat module, so the
   honest answer is "not found" and the caller falls back to its own tables. */
static ABI int32_t module_info_not_found(uint64_t addr, int32_t flags, void *info) {
    (void)addr; (void)flags; (void)info;
    ONCE("sceKernelGetModuleInfoFromAddr/ForUnwind: no per-module info (linked image)");
    return SCE_ERR(E_SRCH);
}
/* Signature unknown upstream as well; reports failure instead of fabricating segment data. */
static ABI int32_t module_segment_info(void *result) {
    (void)result;
    ONCE("sceKernelInternalMemoryGetModuleSegmentInfo: not emulated");
    return SCE_ERR(E_NOENT);
}
typedef struct { uint64_t addr; const char *name; const void *phdr; uint16_t phnum; } PhdrInfo;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr64;
/* __elf_phdr_match_addr(dl_phdr_info*, addr): 1 when addr lies in an executable PT_LOAD segment. */
static ABI int32_t elf_phdr_match_addr(const PhdrInfo *info, const void *addr) {
    if (!info || !info->phdr) return 0;
    const Phdr64 *ph = info->phdr;
    for (uint16_t i = 0; i < info->phnum; ++i) {
        if (ph[i].type != 1 || !(ph[i].flags & 1)) continue;
        uint64_t begin = info->addr + ph[i].vaddr;
        if (begin <= (uint64_t)(uintptr_t)addr && (uint64_t)(uintptr_t)addr + sizeof(addr) < begin + ph[i].memsz) return 1;
    }
    return 0;
}
static ABI int32_t is_signal_return(uint64_t pc) { (void)pc; return 0; }
static ABI void sigreturn_unsupported(void) {
    fputs("STOP: sigreturn is not supported (the port delivers no guest signals)\n", stderr);
    exit(23);
}
static ABI void pthread_cxa_finalize(void *dso) { runtime_finalize(dso); }
static ABI int32_t msync_noop(void *addr, uint64_t length, int32_t flags) { (void)addr; (void)length; (void)flags; return 0; }
/* mmap: only anonymous private memory is supported; file mappings fail loudly with ENODEV. */
#define MAP_ANON_BSD 0x1000
static ABI void *kernel_mmap(void *addr, uint64_t length, int32_t prot, int32_t flags, int32_t fd, int64_t offset) {
    (void)offset;
    if (!(flags & MAP_ANON_BSD) || fd != -1) {
        fprintf(stderr, "Runtime: mmap of a file (fd %d) is not supported\n", fd);
        *runtime_errno() = E_NODEV;
        return (void *)(intptr_t)-1;
    }
    void *where = addr;
    if (runtime_memory_map_anonymous(&where, length, prot)) { *runtime_errno() = 12; return (void *)(intptr_t)-1; }
    return where;
}

/* ---- time ---------------------------------------------------------------------------- */
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } Timesec;
static void zone_now(int64_t *west_sec, int64_t *dst_sec) {
    time_t now = time(NULL);
    struct tm local, utc;
    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
    local.tm_isdst = 0;
    int64_t base = (int64_t)(mktime(&local) - mktime(&utc)); /* standard-time offset east of UTC */
    time_t probe = now; struct tm lt; localtime_r(&probe, &lt);
    *west_sec = -base;
    *dst_sec = lt.tm_isdst > 0 ? 3600 : 0;
}
/* sceKernelConvertUtcToLocaltime(time, *local, *Timesec, *dst_sec) */
static ABI int32_t convert_utc_to_local(int64_t time_utc, int64_t *local, Timesec *st, uint64_t *dst_out) {
    int64_t west, dst;
    zone_now(&west, &dst);
    if (local) *local = time_utc - west + dst;
    if (st) { st->t = time_utc; st->west_sec = (uint32_t)west; st->dst_sec = (uint32_t)dst; }
    if (dst_out) *dst_out = (uint64_t)dst;
    return 0;
}
/* sceKernelConvertLocaltimeToUtc(local, unused, *utc, *Timesec, *dst_sec) */
static ABI int32_t convert_local_to_utc(int64_t local, int64_t unused, int64_t *utc, Timesec *st, int32_t *dst_out) {
    (void)unused;
    int64_t west, dst;
    zone_now(&west, &dst);
    if (utc) *utc = local + west - dst;
    if (st) { st->t = local; st->west_sec = (uint32_t)west; st->dst_sec = (uint32_t)dst; }
    if (dst_out) *dst_out = (int32_t)dst;
    return 0;
}

/* ---- libc internals ----------------------------------------------------------------- */
typedef struct { uint64_t size; uint32_t flag, get_segment_info; uint64_t *mspace_atomic_id_mask, *mstate_table; } HeapInfo;
static uint64_t heap_mask, heap_states[64];
static ABI void libc_heap_trace_info(HeapInfo *info) {
    if (!info) return;
    info->mspace_atomic_id_mask = &heap_mask;
    info->mstate_table = heap_states;
    info->get_segment_info = 0;
}

/* ---- system service, pad, dialogs -------------------------------------------------- */
#define ORBIS_SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME 6
static ABI int32_t system_param_string(int32_t id, char *out, uint64_t size) {
    if (!out || !size) return (int32_t)0x80A10003; /* SCE_SYSTEM_SERVICE_ERROR_PARAMETER */
    if (id != ORBIS_SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME) return (int32_t)0x80A10003;
    snprintf(out, size, "PS4");
    return 0;
}
typedef struct { float ratio; uint8_t reserved[128]; } SafeAreaInfo;
static ABI int32_t safe_area_info(SafeAreaInfo *info) {
    if (!info) return (int32_t)0x80A10003;
    memset(info, 0, sizeof(*info));
    info->ratio = 1.0f; /* no overscan inset */
    return 0;
}
static ABI int32_t pad_set_light_bar(int32_t handle, const void *param) { (void)handle; (void)param; return 0; }
/* ErrorDialog has its own lifecycle/error family; it is not a CommonDialog
 * owner. The error is surfaced in the persistent diagnostic log, then dismissed
 * on UpdateStatus. It has no result/affirmative selection to fabricate. */
#define ERROR_DIALOG_NOT_INITIALIZED ((int32_t)0x80ED0001)
#define ERROR_DIALOG_ALREADY_INITIALIZED ((int32_t)0x80ED0002)
#define ERROR_DIALOG_PARAM_INVALID ((int32_t)0x80ED0003)
#define ERROR_DIALOG_INVALID_STATE ((int32_t)0x80ED0005)
static pthread_mutex_t error_dialog_lock=PTHREAD_MUTEX_INITIALIZER;
static int error_dialog_state;
_Static_assert(sizeof(ErrorDialogParam)==16,"error dialog ABI");
static ABI int32_t error_dialog_init(void) {
    pthread_mutex_lock(&error_dialog_lock);
    int32_t result=error_dialog_state!=DIALOG_NONE ? ERROR_DIALOG_ALREADY_INITIALIZED : 0;
    if (!result) error_dialog_state=DIALOG_INITIALIZED;
    pthread_mutex_unlock(&error_dialog_lock);
    return result;
}
static ABI int32_t error_dialog_open(const ErrorDialogParam *param) {
    pthread_mutex_lock(&error_dialog_lock);
    int32_t result=error_dialog_state!=DIALOG_INITIALIZED && error_dialog_state!=DIALOG_FINISHED ?
        ERROR_DIALOG_INVALID_STATE : !param || param->size!=(int32_t)sizeof(*param) ?
        ERROR_DIALOG_PARAM_INVALID : 0;
    if (!result) {
        printf("Runtime: ErrorDialog error=0x%08x user=%d; logged, no platform dialog UI\n",
               (unsigned)param->error_code,param->user_id);
        error_dialog_state=DIALOG_RUNNING;
    }
    pthread_mutex_unlock(&error_dialog_lock);
    return result;
}
static ABI int32_t error_dialog_status(void) {
    pthread_mutex_lock(&error_dialog_lock);
    if (error_dialog_state==DIALOG_RUNNING) error_dialog_state=DIALOG_FINISHED;
    int status=error_dialog_state;
    pthread_mutex_unlock(&error_dialog_lock);
    return status;
}
static ABI int32_t error_dialog_get_status(void) {
    pthread_mutex_lock(&error_dialog_lock);
    int status=error_dialog_state;
    pthread_mutex_unlock(&error_dialog_lock);
    return status;
}
static ABI int32_t error_dialog_close(void) {
    pthread_mutex_lock(&error_dialog_lock);
    int32_t result=error_dialog_state!=DIALOG_RUNNING ? ERROR_DIALOG_INVALID_STATE : 0;
    if (!result) error_dialog_state=DIALOG_FINISHED;
    pthread_mutex_unlock(&error_dialog_lock);
    return result;
}
static ABI int32_t error_dialog_term(void) {
    pthread_mutex_lock(&error_dialog_lock);
    int32_t result=error_dialog_state==DIALOG_NONE ? ERROR_DIALOG_NOT_INITIALIZED : 0;
    if (!result) error_dialog_state=DIALOG_NONE;
    pthread_mutex_unlock(&error_dialog_lock);
    return result;
}

uintptr_t runtime_ss_resolve(const char *name) {
    uintptr_t evf = runtime_evf_resolve(name);
    if (evf) return evf;
    static const RuntimeExport table[] = {
        {"sceKernelIsNeoMode", kernel_is_neo}, {"sceKernelMlock", kernel_mlock},
        {"sceKernelGetModuleInfoFromAddr", module_info_not_found},
        {"sceKernelGetModuleInfoForUnwind", module_info_not_found},
        {"sceKernelInternalMemoryGetModuleSegmentInfo", module_segment_info},
        {"__elf_phdr_match_addr", elf_phdr_match_addr}, {"_is_signal_return", is_signal_return},
        {"sigreturn", sigreturn_unsupported}, {"__pthread_cxa_finalize", pthread_cxa_finalize},
        {"msync", msync_noop}, {"mmap", kernel_mmap},
        {"sceKernelConvertUtcToLocaltime", convert_utc_to_local},
        {"sceKernelConvertLocaltimeToUtc", convert_local_to_utc},
        {"sceLibcHeapGetTraceInfo", libc_heap_trace_info},
        {"sceSystemServiceParamGetString", system_param_string},
        {"sceSystemServiceGetDisplaySafeAreaInfo", safe_area_info},
        {"scePadSetLightBar", pad_set_light_bar},
        {"sceErrorDialogInitialize", error_dialog_init}, {"sceErrorDialogTerminate", error_dialog_term},
        {"sceErrorDialogOpen", error_dialog_open}, {"sceErrorDialogUpdateStatus", error_dialog_status},
        {"sceErrorDialogGetStatus", error_dialog_get_status}, {"sceErrorDialogClose", error_dialog_close},
    };
    return RUNTIME_LOOKUP(table, name);
}
