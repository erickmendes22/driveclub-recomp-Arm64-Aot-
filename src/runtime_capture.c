/* Capture service availability; no console recording service is exposed.
 * Primary ABI / error reference:
 * shadPS4 e94b9798bc52e2454aac07a99dec39fb248a52ea,
 * src/core/libraries/video_recording/video_recording{.h,.cpp,_error.h}.
 * CUSA00093 1.28 calls SetInfo(0xa007,&zero,4) during initialization and
 * SetInfo(0x0d,&chapter,4) from its enable/disable method, ignoring the result.
 * Returning UNSUPPORTED describes the absent service instead of accepting
 * recording metadata that no recorder will consume.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>

#define RECORDING_INVALID_VALUE ((int32_t)UINT32_C(0x80a80003))
#define RECORDING_UNSUPPORTED ((int32_t)UINT32_C(0x80a80008))

/* Enable/Disable ABI: OpenOrbis 0a1aaf9dd4a92695538bdeb09fb056d06dd11725,
 * include/orbis/ScreenShot.h. There is no confirmed Screenshot-specific
 * unavailable code in that primary header. These are generic runtime kernel
 * operational errors, not invented Screenshot error constants. */
#define CAPTURE_IO_ERROR ((int32_t)UINT32_C(0x80020005))
#define CAPTURE_BUSY ((int32_t)UINT32_C(0x80020010))
#define CAPTURE_UNAVAILABLE ((int32_t)UINT32_C(0x8002004e))
static pthread_mutex_t capture_mutex=PTHREAD_MUTEX_INITIALIZER;
static int (*screenshot_hook)(int enabled);
static uint64_t screenshot_generation;
static int screenshot_in_flight;

void runtime_capture_set_screenshot_hook(int (*hook)(int enabled)) {
    pthread_mutex_lock(&capture_mutex);
    screenshot_hook=hook;
    ++screenshot_generation;
    pthread_mutex_unlock(&capture_mutex);
}
static int32_t set_screenshot_enabled(int enabled) {
    pthread_mutex_lock(&capture_mutex);
    if (screenshot_in_flight) { pthread_mutex_unlock(&capture_mutex); return CAPTURE_BUSY; }
    int (*hook)(int)=screenshot_hook;
    const uint64_t generation=screenshot_generation;
    if (!hook) {
        pthread_mutex_unlock(&capture_mutex);
        static atomic_flag reported=ATOMIC_FLAG_INIT;
        if (!atomic_flag_test_and_set_explicit(&reported,memory_order_relaxed))
            fputs("Capture: screenshot window policy unavailable (runtime ENOSYS)\n",stderr);
        return CAPTURE_UNAVAILABLE;
    }
    screenshot_in_flight=1;
    pthread_mutex_unlock(&capture_mutex);
    /* The Android callback waits for the UI thread. Never hold a runtime lock
     * across JNI, and never block a reentrant UI request behind that callback. */
    const int result=hook(enabled);
    pthread_mutex_lock(&capture_mutex);
    screenshot_in_flight=0;
    const int retired=generation!=screenshot_generation;
    pthread_mutex_unlock(&capture_mutex);
    if (result || retired) {
        fputs("Capture: screenshot window policy was not confirmed (runtime EIO)\n",stderr);
        return CAPTURE_IO_ERROR;
    }
    return 0;
}
static ABI int32_t screenshot_enable(void) { return set_screenshot_enabled(1); }
static ABI int32_t screenshot_disable(void) { return set_screenshot_enabled(0); }

static int known_recording_info(uint32_t kind) {
    switch (kind) {
    case 0x2:    /* Subtitle */
    case 0x6:    /* Description */
    case 0x7:    /* Comments */
    case 0x8:    /* Keywords */
    case 0xd:    /* Chapter */
    case 0xa01:  /* Copyright */
    case 0xa007: /* PermissionLevel */
    case 0xa008: /* GuardArea */
    case 0xa009: /* UserMeta */
        return 1;
    default:
        return 0;
    }
}
static ABI int32_t recording_set_info(uint32_t kind,const void *info,uint64_t length) {
    if (!info || !known_recording_info(kind)) return RECORDING_INVALID_VALUE;
    /* The primary implementation delegates length/payload validation to
     * libSceAvcap. With no such backend, do not read an unconfirmed layout,
     * retain the caller's buffer or invent extra accepted payload semantics. */
    static atomic_flag reported=ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set_explicit(&reported,memory_order_relaxed))
        fprintf(stderr,"Capture: sceVideoRecordingSetInfo(kind=%#x, size=%" PRIu64 "): recording service unavailable (0x80a80008)\n",
                kind,length);
    return RECORDING_UNSUPPORTED;
}

static const RuntimeExport exports[]={
    {"sceVideoRecordingSetInfo",recording_set_info},
    {"sceScreenShotEnable",screenshot_enable},
    {"sceScreenShotDisable",screenshot_disable},
};
uintptr_t runtime_capture_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
