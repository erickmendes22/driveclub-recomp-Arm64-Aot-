/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Android controllers enter through Pad; this USB discovery backend exposes no
 * raw devices. It implements discovery and event waiting, not USB passthrough.
 * ABI/error domain: shadPS4 e94b9798bc52e2454aac07a99dec39fb248a52ea,
 * src/core/libraries/usbd/usbd.{h,cpp}; OpenOrbis Usbd.h.
 * The eboot ignores sceUsbdInit's return value and runs an event thread even
 * with an empty list, so returning an immediate error from every call spins.
 * Transfer APIs are deliberately left unresolved until an actual backend exists. */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

#define USB_INVALID ((int32_t)UINT32_C(0x80240002))
#define USB_NO_MEMORY ((int32_t)UINT32_C(0x8024000b))
#define USB_OTHER ((int32_t)UINT32_C(0x802400ff))
typedef struct { int64_t seconds, microseconds; } UsbTimeval;
typedef struct UsbList { void **guest; struct UsbList *next; } UsbList;
static pthread_mutex_t usb_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t usb_changed;
static pthread_once_t usb_once=PTHREAD_ONCE_INIT;
static clockid_t usb_clock=CLOCK_REALTIME;
static int usb_cond_error;
static unsigned usb_references;
static UsbList *usb_lists;

static void usb_create_cond(void) {
    pthread_condattr_t attr;
    usb_cond_error=pthread_condattr_init(&attr);
    if (usb_cond_error) return;
#ifndef _WIN32
    if (!pthread_condattr_setclock(&attr,CLOCK_MONOTONIC)) usb_clock=CLOCK_MONOTONIC;
#endif
    usb_cond_error=pthread_cond_init(&usb_changed,&attr);
    pthread_condattr_destroy(&attr);
}
static ABI int32_t usb_init(void) {
    pthread_once(&usb_once,usb_create_cond);
    if (usb_cond_error) return USB_OTHER;
    pthread_mutex_lock(&usb_mutex);
    if (usb_references==UINT_MAX) { pthread_mutex_unlock(&usb_mutex); return USB_OTHER; }
    if (!usb_references++)
        puts("USB: discovery initialized with no raw USB devices; controls use Android Pad input");
    pthread_mutex_unlock(&usb_mutex);
    return 0;
}
static ABI void usb_exit(void) {
    pthread_mutex_lock(&usb_mutex);
    if (usb_references && !--usb_references) pthread_cond_broadcast(&usb_changed);
    pthread_mutex_unlock(&usb_mutex);
}
static ABI int64_t usb_get_list(void ***out) {
    if (!out) return USB_INVALID;
    *out=NULL;
    pthread_mutex_lock(&usb_mutex);
    if (!usb_references) { pthread_mutex_unlock(&usb_mutex); return USB_OTHER; }
    UsbList *list=malloc(sizeof(*list));
    if (list) list->guest=runtime_low_map(sizeof(void *),PROT_READ|PROT_WRITE);
    if (!list || !list->guest) {
        free(list); pthread_mutex_unlock(&usb_mutex); return USB_NO_MEMORY;
    }
    list->guest[0]=NULL;
    list->next=usb_lists; usb_lists=list;
    *out=list->guest;
    pthread_mutex_unlock(&usb_mutex);
    return 0; /* An allocated, NULL-terminated list containing zero devices. */
}
static ABI void usb_free_list(void **guest, int32_t unref_devices) {
    (void)unref_devices; /* There are no device references in an empty list. */
    if (!guest) return;
    pthread_mutex_lock(&usb_mutex);
    UsbList **at=&usb_lists;
    while (*at && (*at)->guest!=guest) at=&(*at)->next;
    UsbList *list=*at;
    if (list) *at=list->next;
    pthread_mutex_unlock(&usb_mutex);
    if (list) {
        runtime_low_unmap(list->guest,sizeof(void *));
        free(list);
    }
}
static ABI int32_t usb_handle_events(const UsbTimeval *timeout) {
    if (!timeout || timeout->seconds<0 || timeout->microseconds<0 || timeout->microseconds>=1000000)
        return USB_INVALID;
    pthread_once(&usb_once,usb_create_cond);
    if (usb_cond_error) return USB_OTHER;
    struct timespec end;
    if (clock_gettime(usb_clock,&end)) return USB_OTHER;
    if (timeout->seconds>INT64_MAX-(int64_t)end.tv_sec-1) return USB_INVALID;
    end.tv_sec+=(time_t)timeout->seconds;
    end.tv_nsec+=(long)timeout->microseconds*1000;
    if (end.tv_nsec>=1000000000) { ++end.tv_sec; end.tv_nsec-=1000000000; }
    pthread_mutex_lock(&usb_mutex);
    if (!usb_references) { pthread_mutex_unlock(&usb_mutex); return USB_OTHER; }
    int error=0;
    while (usb_references && !error)
        error=pthread_cond_timedwait(&usb_changed,&usb_mutex,&end);
    pthread_mutex_unlock(&usb_mutex);
    return !error || error==ETIMEDOUT ? 0 : USB_OTHER;
}
static const RuntimeExport exports[]={
    {"sceUsbdInit",usb_init}, {"sceUsbdExit",usb_exit},
    {"sceUsbdGetDeviceList",usb_get_list}, {"sceUsbdFreeDeviceList",usb_free_list},
    {"sceUsbdHandleEventsTimeout",usb_handle_events},
};
uintptr_t runtime_usbd_resolve(const char *name) {
    return runtime_lookup(exports,sizeof(exports)/sizeof(*exports),name);
}
