/* Offline AppContent provider for the configured base-game profile.
 * No package/DLC mounting, downloads, entitlement or license emulation.
 * The existing private /download0 filesystem is queryable and formattable. */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#endif
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
#define NOT_INITIALIZED ((int32_t)0x80d90001)
#define NOT_MOUNTED ((int32_t)0x80d90004)
#define NOT_FOUND ((int32_t)0x80d90005)
#define NO_ENTITLEMENT ((int32_t)0x80d90007)
#define INTERNAL ((int32_t)0x80d9000a)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* HLE system-module references. Taking a reference does not certify that all
 * its imports exist: the loader's import preflight reports that separately.
 * AppContent (0xb4) additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x referenced (HLE imports checked separately)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    if (!configured || !refs) return NOT_INITIALIZED;
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}
static int content_ready(void) { return configured && refs && initialized; }
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    if (!content_ready()) return NOT_INITIALIZED;
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    if (!content_ready()) return NOT_INITIALIZED;
    if (service>7 || (capacity && !list) || (!list && !hits)) return PARAMETER;
    /* No add-ons are mounted by this provider. Never touch unused list slots. */
    if (hits) *hits=0;
    ++lists; return 0;
}
typedef struct { char data[16]; } ContentMount;
static ABI int32_t addon_info(uint32_t service,const void *entitlement,void *info) {
    if (!content_ready()) return NOT_INITIALIZED;
    if (service>7 || !entitlement || !info) return PARAMETER;
    return NO_ENTITLEMENT;
}
static ABI int32_t addon_mount(uint32_t service,const void *entitlement,ContentMount *mount) {
    if (!content_ready()) return NOT_INITIALIZED;
    if (service>7 || !entitlement || !mount) return PARAMETER;
    /* No add-on was installed by this base-game provider. Leave the output
     * untouched: it must never look like a usable mount on failure. */
    return NOT_FOUND;
}
static ABI int32_t addon_unmount(const ContentMount *mount) {
    if (!content_ready()) return NOT_INITIALIZED;
    if (!mount || !memchr(mount->data,0,sizeof(mount->data))) return PARAMETER;
    return NOT_MOUNTED;
}
static int32_t download_path(const ContentMount *mount,char *path,size_t size) {
    if (!content_ready()) return NOT_INITIALIZED;
    if (!mount || !memchr(mount->data,0,sizeof(mount->data))) return PARAMETER;
    if (strcmp(mount->data,"/download0")) return NOT_FOUND;
    if (runtime_file_translate("/download0",path,size)) return NOT_FOUND;
    return 0;
}
static int32_t content_io_error(int error,const char *operation) {
    fprintf(stderr,"Runtime: AppContent %s failed: %s (errno %d)\n",operation,strerror(error),error);
    return error==ENOENT ? NOT_FOUND : error==EBUSY ? BUSY : INTERNAL;
}
static pthread_mutex_t download_lock=PTHREAD_MUTEX_INITIALIZER;
#ifndef _WIN32
/* Walk directory descriptors, never symlinks. The root mount stays present;
 * a link inside download0 is unlinked without following its target. */
static int clear_download_directory(int fd) {
    int copy=dup(fd);
    if (copy<0) return errno;
    DIR *directory=fdopendir(copy);
    if (!directory) { int error=errno; close(copy); return error; }
    int error=0;
    for (;;) {
        errno=0;
        struct dirent *entry=readdir(directory);
        if (!entry) { if (errno) error=errno; break; }
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        struct stat info;
        if (fstatat(fd,entry->d_name,&info,AT_SYMLINK_NOFOLLOW)) { error=errno; break; }
        if (S_ISDIR(info.st_mode)) {
            int child=openat(fd,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if (child<0) { error=errno; break; }
            error=clear_download_directory(child);
            close(child);
            if (error) break;
            if (unlinkat(fd,entry->d_name,AT_REMOVEDIR)) { error=errno; break; }
        } else if (unlinkat(fd,entry->d_name,0)) { error=errno; break; }
    }
    closedir(directory);
    return error;
}
#endif
static ABI int32_t download_format(const ContentMount *mount) {
    char path[1024];
    int32_t result=download_path(mount,path,sizeof(path));
    if (result) return result;
    pthread_mutex_lock(&download_lock);
#ifdef _WIN32
    DWORD attributes=GetFileAttributesA(path);
    if (attributes==INVALID_FILE_ATTRIBUTES) result=NOT_FOUND;
    else if (!(attributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) result=PARAMETER;
    else if (compat_remove_tree(path) || mkdir(path,0755)) result=content_io_error(errno,"format");
#else
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) result=content_io_error(errno,"format open");
    else {
        int error=clear_download_directory(fd);
        close(fd);
        if (error) result=content_io_error(error,"format");
    }
#endif
    pthread_mutex_unlock(&download_lock);
    if (!result) puts("Runtime: AppContent formatted private /download0");
    return result;
}
static ABI int32_t download_space(const ContentMount *mount,uint64_t *kilobytes) {
    if (!kilobytes) return PARAMETER;
    char path[1024];
    int32_t result=download_path(mount,path,sizeof(path));
    if (result) return result;
    uint64_t bytes=0;
#ifdef _WIN32
    DWORD attributes=GetFileAttributesA(path);
    if (attributes==INVALID_FILE_ATTRIBUTES) return NOT_FOUND;
    if (!(attributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return PARAMETER;
    ULARGE_INTEGER available;
    if (!GetDiskFreeSpaceExA(path,&available,NULL,NULL)) return INTERNAL;
    bytes=available.QuadPart;
#else
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return content_io_error(errno,"space open");
    struct statvfs info;
    int error=fstatvfs(fd,&info) ? errno : 0;
    close(fd);
    if (error) return content_io_error(error,"space");
    if (info.f_frsize && info.f_bavail>UINT64_MAX/info.f_frsize) return INTERNAL;
    bytes=(uint64_t)info.f_bavail*info.f_frsize;
#endif
    *kilobytes=bytes/1024;
    return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (nid_match(name, "g8cM39EUZ6o#M#N")) return (uintptr_t)module_load;
    if (nid_match(name, "fMP5NHUOaMk#M#N")) return (uintptr_t)module_loaded;
    if (nid_match(name, "eR2bZFAAU0Q#M#N")) return (uintptr_t)module_unload;
    if (nid_match(name, "R9lA82OraNs#c#d")) return (uintptr_t)content_init;
    if (nid_match(name, "99b82IKXpH4#c#d")) return (uintptr_t)param_int;
    if (nid_match(name, "xnd8BJzAxmk#c#d")) return (uintptr_t)addon_list;
    static const RuntimeExport exports[]={
        {"sceAppContentGetAddcontInfo",addon_info},
        {"sceAppContentAddcontMount",addon_mount}, {"sceAppContentAddcontUnmount",addon_unmount},
        {"sceAppContentDownloadDataFormat",download_format},
        {"sceAppContentDownloadDataGetAvailableSpaceKb",download_space},
    };
    return RUNTIME_LOOKUP(exports,name);
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
