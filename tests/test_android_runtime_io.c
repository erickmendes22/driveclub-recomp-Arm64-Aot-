/* Native contract test for Android provider descriptors and pad input.
 * Build on a POSIX host without the Android SDK:
 * cc -std=c11 -D__ANDROID__ -DDRIVECLUB_ANDROID_RUNTIME=1 -I. -Isrc \
 *   tests/test_android_runtime_io.c src/runtime_file.c src/runtime_pad_android.c \
 *   -pthread -lm -o /tmp/test-android-io && /tmp/test-android-io
 * Providers below are test fixtures. Production callbacks live in runtime_bridge.cpp. */
#define _GNU_SOURCE
#include "src/runtime.h"
#include "src/runtime_android.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

typedef struct { int64_t sec, usec; } TestTimeval;

static char root[512],archive[600];
static int last_provider_fd=-1,provider_opens,provider_resolves;
static _Thread_local int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int value) { return value; }
int runtime_benchmark_marker(const char *path) { (void)path; return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
void runtime_exit_tick(void) {}
int runtime_exit_captures(void) { return 0; }
void bbgpu_reflex_pre_input_sample(void) {}
void bbgpu_reflex_on_input_sampled(void) {}
int bbgpu_overlay_captures_input(void) { return 0; }
int driveclub_android_files_active(void) { return 1; }
int driveclub_android_is_game_path(const char *path) {
    return path && (*path!='/' || (!strncmp(path,"/app0",5) && (!path[5] || path[5]=='/')));
}
int driveclub_android_open(const char *path) {
    if (strstr(path,"..")) return -EACCES;
    if (strcmp(path,"/app0/game000.dat")) return -ENOENT;
    ++provider_opens;
    last_provider_fd=open(archive,O_RDONLY|O_CLOEXEC);
    return last_provider_fd<0 ? -errno : last_provider_fd;
}
int driveclub_android_stat(const char *path,struct stat *out) {
    if (strstr(path,"..")) return EACCES;
    if (!strcmp(path,"/app0")) return stat(root,out) ? errno : 0;
    if (!strcmp(path,"/app0/game000.dat")) return stat(archive,out) ? errno : 0;
    return ENOENT;
}
int driveclub_android_list(const char *path,char ***out,size_t *count) {
    if (strcmp(path,"/app0")) return ENOENT;
    *out=calloc(1,sizeof(char *)); assert(*out);
    (*out)[0]=strdup("f:game000.dat"); assert((*out)[0]); *count=1; return 0;
}
void driveclub_android_free_list(char **entries,size_t count) {
    for (size_t i=0;i<count;++i) free(entries[i]);
    free(entries);
}
int driveclub_android_resolve(const char *path,char *out,size_t size) {
    ++provider_resolves;
    if (strcmp(path,"/app0/game000.dat")) return ENOENT;
    return (size_t)snprintf(out,size,"%s",archive)>=size ? ENAMETOOLONG : 0;
}

static void test_provider_io(void) {
    char template[]="/tmp/driveclub-android-io-XXXXXX";
    const char *directory=mkdtemp(template); assert(directory);
    snprintf(root,sizeof(root),"%s",directory);
    snprintf(archive,sizeof(archive),"%s/game000.dat",root);
    int source=open(archive,O_CREAT|O_RDWR|O_EXCL,0600); assert(source>=0);
    const int64_t offset=INT64_C(5)*1024*1024*1024+37;
    assert(pwrite(source,"DRIVECLUB",9,offset)==9); close(source);
    char user[600]; snprintf(user,sizeof(user),"%s/user",root);
    runtime_file_configure("/metadata-only",user);

    int game=(int)runtime_file_open("/app0/game000.dat",0,0); assert(game>=3);
    int64_t (ABI *sce_futimes)(int,const TestTimeval *)=(void *)runtime_file_resolve("sceKernelFutimes");
    int64_t (ABI *posix_futimes)(int,const TestTimeval *)=(void *)runtime_file_resolve("futimes");
    assert(sce_futimes && posix_futimes);
    const TestTimeval times[]={{1700000000,123456},{1700000100,654321}};
    struct stat archive_before,archive_after;
    assert(!stat(archive,&archive_before));
    assert(sce_futimes(game,times)==(int32_t)UINT32_C(0x8002001e));
    assert(!stat(archive,&archive_after));
    assert(archive_before.st_mtim.tv_sec==archive_after.st_mtim.tv_sec
            && archive_before.st_mtim.tv_nsec==archive_after.st_mtim.tv_nsec);
    char bytes[10]={0};
    assert(runtime_file_pread(game,bytes,9,offset)==9);
    assert(!strcmp(bytes,"DRIVECLUB"));
    assert(runtime_file_lseek(game,offset,SEEK_SET)==offset);
    memset(bytes,0,sizeof(bytes)); assert(runtime_file_read(game,bytes,9)==9);
    assert(!strcmp(bytes,"DRIVECLUB"));
    assert(provider_opens==1 && provider_resolves==0);
    const int owned=last_provider_fd;
    assert(runtime_file_close(game)==0);
    assert(fcntl(owned,F_GETFD)==-1 && errno==EBADF);
    assert(runtime_file_open("/app0/game000.dat",2,0)==-EROFS);
    assert(runtime_file_open("/app0/../secret",0,0)==-EACCES);
    assert(runtime_file_open("/app0/missing",0,0)==-ENOENT);
    assert(provider_opens==1);

    int dir=(int)runtime_file_open("/app0",0x20000,0); assert(dir>=3);
    unsigned char guest_stat[120]; assert(runtime_file_fstat(dir,guest_stat)==0);
    char listing[1024];
    int64_t n=runtime_file_getdents(dir,listing,sizeof(listing),NULL); assert(n>0);
    int found=0;
    for (int64_t at=0;at<n;) {
        uint16_t stride; memcpy(&stride,listing+at+4,2); assert(stride>=8);
        if (!strcmp(listing+at+8,"game000.dat")) { assert(listing[at+6]==8); ++found; }
        at+=stride;
    }
    assert(found==1); assert(runtime_file_close(dir)==0);
    int save=(int)runtime_file_open("/data/save.bin",0x202,0600); assert(save>=3);
    assert(runtime_file_write(save,"save",4)==4);
    char save_path[700]; snprintf(save_path,sizeof(save_path),"%s/data/save.bin",user);
    assert(sce_futimes(save,times)==0);
    struct stat saved;
    assert(!stat(save_path,&saved));
    assert(saved.st_atim.tv_sec==times[0].sec && saved.st_atim.tv_nsec==times[0].usec*1000);
    assert(saved.st_mtim.tv_sec==times[1].sec && saved.st_mtim.tv_nsec==times[1].usec*1000);
    TestTimeval invalid[]={{1,1000000},{2,0}};
    assert(sce_futimes(save,invalid)==(int32_t)UINT32_C(0x80020016));
    assert(posix_futimes(save,invalid)==-1 && guest_errno==EINVAL);
    assert(!stat(save_path,&saved) && saved.st_mtim.tv_sec==times[1].sec);
    time_t before=time(NULL);
    assert(sce_futimes(save,NULL)==0);
    assert(!stat(save_path,&saved) && saved.st_mtim.tv_sec>=before && saved.st_mtim.tv_sec<=time(NULL));
    assert(runtime_file_close(save)==0);
    assert(sce_futimes(save,times)==(int32_t)UINT32_C(0x80020009));
    unlink(save_path);
    snprintf(save_path,sizeof(save_path),"%s/data",user); rmdir(save_path);
    snprintf(save_path,sizeof(save_path),"%s/download0",user); rmdir(save_path);
    snprintf(save_path,sizeof(save_path),"%s/temp0",user); rmdir(save_path);
    rmdir(user); unlink(archive); rmdir(root);
}

static void test_pad(void) {
    int32_t (*init)(void)=(void *)runtime_pad_resolve("scePadInit");
    int32_t (*open_pad)(int,int,int,const void *)=(void *)runtime_pad_resolve("scePadOpen");
    int32_t (*read_pad)(int,void *)=(void *)runtime_pad_resolve("scePadReadState");
    int32_t (*close_pad)(int)=(void *)runtime_pad_resolve("scePadClose");
    assert(init && open_pad && read_pad && close_pad);
    assert(init()==0);
    const int handle=open_pad(1,0,0,NULL); assert(handle>0);
    assert(open_pad(1,0,0,NULL)<0);
    runtime_android_pad_update(0x4000,-1,1,0,0,0.5f,1);
    unsigned char data[120]; assert(read_pad(handle,data)==0);
    uint32_t buttons; memcpy(&buttons,data,4);
    assert((buttons&0x4300)==0x4300);
    assert(data[4]==0 && data[5]==255 && data[6]==128 && data[7]==128);
    assert(data[8]==128 && data[9]==255);
    runtime_android_pad_update(0,0,0,0,0,0,0);
    assert(read_pad(handle,data)==0); memcpy(&buttons,data,4); assert(buttons==0);
    assert(close_pad(handle)==0 && read_pad(handle,data)<0);
}
int main(void) {
    test_provider_io(); test_pad();
    puts("Android runtime IO passed: 64-bit archive reads, FD ownership, read-only trees, directories, saves, futimes and controller state.");
    return 0;
}
