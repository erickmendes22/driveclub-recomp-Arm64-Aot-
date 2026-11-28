/* Contract tests through the production component resolvers and runtime_lookup.
 * No game files, renderer, network session or real user installation is used.
 * See tools/README-service-contracts.md for the reproducible build command. */
#define _GNU_SOURCE
#include "src/runtime.h"
#include "src/runtime_dialogs.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* Only unrelated renderer/kernel facilities are isolated. None supplies a
 * successful result for a service under test. The four tested resolvers and
 * the NID/name table are linked from the production translation units. */
uintptr_t bbgpu_resolve(const char *name) { (void)name; assert(!"unexpected GPU resolution"); return 0; }
void bbgpu_overlay_trophy(int id,int grade,const char *title,const char *description) {
    (void)id; (void)grade; (void)title; (void)description; assert(!"unexpected trophy UI");
}
int bbgpu_text_input_begin(const char *initial,const char *prompt) { (void)initial; (void)prompt; return 0; }
int bbgpu_text_input_poll(char *out,uint64_t size) { (void)out; (void)size; assert(!"unexpected IME poll"); return 0; }
uintptr_t runtime_evf_resolve(const char *name) { (void)name; return 0; }
int runtime_memory_map_anonymous(void **out,uint64_t size,int prot) {
    (void)out; (void)size; (void)prot; assert(!"unexpected kernel mmap"); return -1;
}
int32_t *runtime_errno(void) { static _Thread_local int32_t value; return &value; }
const char *runtime_savedata_title(void) { return "TEST00000"; }
int runtime_benchmark_marker(const char *path) { (void)path; return 0; }

static const struct { const char *nid,*name; } names[]={
#include "src/import_names.inc"
};
static uintptr_t resolve(const char *symbol) {
    const char *nid=NULL;
    for (size_t i=0;i<sizeof(names)/sizeof(*names);++i)
        if (!strcmp(symbol,names[i].name)) { nid=names[i].nid; break; }
    assert(nid);
    char scoped[96]; snprintf(scoped,sizeof(scoped),"%s#arbitrary#tags",nid);
    uintptr_t address=runtime_content_resolve(scoped);
    if (!address) address=runtime_services_resolve(scoped);
    if (!address) address=runtime_rtc_resolve(scoped);
    if (!address) address=runtime_ss_resolve(scoped);
    if (!address) fprintf(stderr,"FAIL: unresolved service %s (%s)\n",symbol,nid);
    assert(address);
    return address;
}
typedef int32_t (ABI *Fn0)(void);
typedef int32_t (ABI *FnPointer)(void *);
static int32_t call0(const char *name) { return ((Fn0)resolve(name))(); }
static int32_t callp(const char *name,void *param) { return ((FnPointer)resolve(name))(param); }
static void unchanged(const void *buffer,size_t size,unsigned char value) {
    const unsigned char *bytes=buffer;
    for (size_t i=0;i<size;++i) assert(bytes[i]==value);
}
static MessageDialogParam message_param(unsigned mode) {
    MessageDialogParam p={.base={.size=48},.size=136,.mode=mode,.user_id=1};
    return p;
}
static void test_messages(void) {
    struct { unsigned char before[8]; MessageDialogResult result; unsigned char after[8]; } guard;
    memset(&guard,0xa5,sizeof(guard));
    assert(call0("sceMsgDialogInitialize")==DIALOG_NOT_SYSTEM_INITIALIZED);
    assert(!call0("sceCommonDialogIsUsed"));
    assert(!call0("sceCommonDialogInitialize"));
    assert(call0("sceCommonDialogInitialize")==DIALOG_ALREADY_SYSTEM_INITIALIZED);
    assert(!call0("sceMsgDialogInitialize"));
    assert(call0("sceCommonDialogIsUsed")==1);
    assert(call0("sceMsgDialogInitialize")==DIALOG_ALREADY_INITIALIZED);
    assert(call0("sceSaveDataDialogInitialize")==DIALOG_BUSY);
    assert(callp("sceMsgDialogGetResult",&guard.result)==DIALOG_NOT_FINISHED);
    unchanged(&guard,sizeof(guard),0xa5);
    assert(callp("sceMsgDialogOpen",NULL)==DIALOG_ARG_NULL);
    MessageUserParam user={.button_type=1,.message="Choose an action"};
    MessageDialogParam p=message_param(1); p.user=&user;
    p.size=135;
    assert(callp("sceMsgDialogOpen",&p)==DIALOG_PARAM_INVALID);
    assert(call0("sceMsgDialogUpdateStatus")==DIALOG_INITIALIZED);
    p.size=136;
    assert(!callp("sceMsgDialogOpen",&p));
    assert(callp("sceMsgDialogGetResult",&guard.result)==DIALOG_NOT_FINISHED);
    assert(callp("sceMsgDialogOpen",&p)==DIALOG_INVALID_STATE);
    assert(call0("sceMsgDialogUpdateStatus")==DIALOG_FINISHED);
    assert(callp("sceMsgDialogGetResult",NULL)==DIALOG_ARG_NULL);
    assert(!callp("sceMsgDialogGetResult",&guard.result));
    assert(guard.result.mode==0 && guard.result.result==DIALOG_RESULT_CANCELED && guard.result.button==0);
    unchanged(guard.result.reserved,sizeof(guard.result.reserved),0);
    unchanged(guard.before,sizeof(guard.before),0xa5); unchanged(guard.after,sizeof(guard.after),0xa5);
    assert(call0("sceMsgDialogClose")==DIALOG_NOT_RUNNING);
    user.button_type=4; /* Actual legacy eboot request, followed by status polling. */
    assert(!callp("sceMsgDialogOpen",&p));
    assert(!call0("sceMsgDialogClose"));
    assert(!callp("sceMsgDialogGetResult",&guard.result));
    assert(guard.result.result==DIALOG_RESULT_CANCELED && !guard.result.button);
    user.button_type=1;

    MessageProgressParam progress={.message="Loading"};
    p=message_param(2); p.progress=&progress;
    assert(!callp("sceMsgDialogOpen",&p));
    assert(call0("sceMsgDialogUpdateStatus")==DIALOG_RUNNING);
    assert(call0("sceMsgDialogUpdateStatus")==DIALOG_RUNNING);
    assert(!call0("sceMsgDialogClose"));
    assert(!callp("sceMsgDialogGetResult",&guard.result));
    assert(guard.result.result==DIALOG_RESULT_OK && !guard.result.button);
    assert(!call0("sceMsgDialogTerminate"));
    assert(!call0("sceCommonDialogIsUsed"));
    assert(call0("sceMsgDialogTerminate")==DIALOG_NOT_INITIALIZED);
}
static void test_save_dialogs(void) {
    typedef int32_t (ABI *Progress)(uint32_t,uint32_t);
    Progress set=(Progress)resolve("sceSaveDataDialogProgressBarSetValue");
    assert(!call0("sceSaveDataDialogIsReadyToDisplay"));
    assert(!call0("sceSaveDataDialogInitialize"));
    assert(call0("sceSaveDataDialogIsReadyToDisplay")==1);
    unsigned char items[96]={0}, directory[32], metadata[1328];
    memset(directory,0x6a,sizeof(directory)); memset(metadata,0x7b,sizeof(metadata));
    SaveProgressParam progress={.system_type=1};
    int cookie=123;
    SaveDialogParam p={.base={.size=48},.size=152,.mode=5,.display_type=1,
        .items=items,.progress=&progress,.user_data=&cookie};
    struct { uint64_t before; SaveDialogResult result; uint64_t after; } guard;
    memset(&guard,0xa5,sizeof(guard));
    guard.result.dir_name=directory; guard.result.param=metadata;
    p.base.size=47;
    assert(callp("sceSaveDataDialogOpen",&p)==DIALOG_PARAM_INVALID);
    p.base.size=48;
    assert(!callp("sceSaveDataDialogOpen",&p));
    assert(set(1,25)==DIALOG_PARAM_INVALID && set(0,101)==DIALOG_PARAM_INVALID);
    assert(!set(0,25) && !set(0,100));
    assert(call0("sceSaveDataDialogUpdateStatus")==DIALOG_RUNNING);
    assert(callp("sceSaveDataDialogGetResult",&guard.result)==DIALOG_NOT_FINISHED);
    assert(!call0("sceSaveDataDialogClose"));
    assert(call0("sceSaveDataDialogUpdateStatus")==DIALOG_FINISHED);
    assert(!callp("sceSaveDataDialogGetResult",&guard.result));
    assert(guard.result.mode==5 && guard.result.result==DIALOG_RESULT_OK && !guard.result.button);
    assert(guard.result.user_data==&cookie && guard.result.dir_name==directory && guard.result.param==metadata);
    assert(guard.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && guard.after==guard.before);
    unchanged(guard.result.reserved,sizeof(guard.result.reserved),0);
    unchanged(directory,sizeof(directory),0x6a); unchanged(metadata,sizeof(metadata),0x7b);
    assert(set(0,1)==DIALOG_NOT_RUNNING);

    p.mode=1; p.progress=NULL;
    assert(!callp("sceSaveDataDialogOpen",&p));
    assert(set(0,1)==DIALOG_NOT_SUPPORTED);
    assert(call0("sceSaveDataDialogUpdateStatus")==DIALOG_FINISHED);
    assert(!callp("sceSaveDataDialogGetResult",&guard.result));
    assert(guard.result.mode==1 && guard.result.result==DIALOG_RESULT_CANCELED && !guard.result.button);
    unchanged(directory,sizeof(directory),0x6a); unchanged(metadata,sizeof(metadata),0x7b);
    assert(guard.result.user_data==&cookie);
    assert(!call0("sceSaveDataDialogTerminate"));
    assert(!call0("sceCommonDialogIsUsed") && !call0("sceSaveDataDialogIsReadyToDisplay"));
}
static void test_profile_and_error(void) {
    assert(!call0("sceNpProfileDialogInitialize"));
    int cookie=123;
    ProfileDialogParam p={.base={.size=48},.size=128,.mode=1,.user_id=1,.user_data=&cookie};
    memcpy(p.online_id,"test",5);
    struct { uint64_t before; ProfileDialogResult result; uint64_t after; } guard;
    memset(&guard,0xa5,sizeof(guard));
    assert(!callp("sceNpProfileDialogOpen",&p));
    assert(call0("sceNpProfileDialogUpdateStatus")==DIALOG_FINISHED);
    assert(!callp("sceNpProfileDialogGetResult",&guard.result));
    assert(guard.result.result==(int32_t)0x80550006 && guard.result.user_action==DIALOG_RESULT_CANCELED);
    assert(guard.result.user_data==&cookie);
    unchanged(guard.result.reserved,sizeof(guard.result.reserved),0);
    assert(guard.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && guard.after==guard.before);
    assert(!call0("sceNpProfileDialogTerminate"));
    assert(!call0("sceNpCommerceDialogInitialize"));
    CommerceDialogParam commerce={.base={.size=48},.size=128,.user_id=1,.mode=5,.user_data=&cookie};
    struct { uint64_t before; CommerceDialogResult result; uint64_t after; } commerce_guard;
    memset(&commerce_guard,0xa5,sizeof(commerce_guard));
    assert(callp("sceNpCommerceDialogGetResult",NULL)==DIALOG_ARG_NULL);
    assert(callp("sceNpCommerceDialogGetResult",&commerce_guard.result)==DIALOG_NOT_FINISHED);
    assert(!callp("sceNpCommerceDialogOpen",&commerce));
    assert(call0("sceNpCommerceDialogUpdateStatus")==DIALOG_FINISHED);
    assert(callp("sceNpCommerceDialogGetResult",&commerce_guard.result)==DIALOG_RESULT_CANCELED);
    assert(commerce_guard.result.result==DIALOG_RESULT_CANCELED && !commerce_guard.result.authorized);
    assert(commerce_guard.result.user_data==&cookie);
    unchanged(commerce_guard.result.reserved,sizeof(commerce_guard.result.reserved),0);
    assert(commerce_guard.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && commerce_guard.after==commerce_guard.before);
    assert(!call0("sceNpCommerceDialogTerminate"));

    assert(call0("sceErrorDialogTerminate")== (int32_t)0x80ED0001);
    assert(call0("sceErrorDialogClose")== (int32_t)0x80ED0005);
    assert(callp("sceErrorDialogOpen",NULL)==(int32_t)0x80ED0005);
    assert(!call0("sceErrorDialogInitialize"));
    assert(call0("sceErrorDialogInitialize")== (int32_t)0x80ED0002);
    assert(call0("sceErrorDialogUpdateStatus")==DIALOG_INITIALIZED);
    assert(callp("sceErrorDialogOpen",NULL)==(int32_t)0x80ED0003);
    ErrorDialogParam error={.size=15,.error_code=(int32_t)0x80550006,.user_id=1};
    assert(callp("sceErrorDialogOpen",&error)==(int32_t)0x80ED0003);
    error.size=16;
    assert(!callp("sceErrorDialogOpen",&error));
    assert(!call0("sceErrorDialogClose"));
    assert(call0("sceErrorDialogUpdateStatus")==DIALOG_FINISHED);
    assert(!callp("sceErrorDialogOpen",&error));
    assert(call0("sceErrorDialogUpdateStatus")==DIALOG_FINISHED);
    assert(!call0("sceErrorDialogTerminate"));
    assert(call0("sceErrorDialogUpdateStatus")==DIALOG_NONE);
}
static void test_voice_and_unavailable_input(void) {
    typedef struct { int32_t type; uint16_t threshold,mute; float volume; uint32_t data[3]; } PortParam;
    typedef struct {
        int32_t type,state; uint32_t *edges; uint32_t bytes,frame_size;
        uint16_t edge_count,reserved; uint32_t pad;
    } PortInfo;
    typedef int32_t (ABI *Init)(const void *,int32_t);
    typedef int32_t (ABI *Create)(int32_t *,const PortParam *);
    typedef int32_t (ABI *Info)(uint32_t,PortInfo *);
    typedef int32_t (ABI *SetVolume)(uint32_t,float);
    typedef int32_t (ABI *GetVolume)(uint32_t,float *);
    typedef int32_t (ABI *PortFn)(uint32_t);
    typedef int32_t (ABI *Read)(uint32_t,void *,uint32_t *);
    typedef int32_t (ABI *Write)(uint32_t,const void *,uint32_t *,int16_t);
    Init init=(Init)resolve("sceVoiceInit");
    Create create=(Create)resolve("sceVoiceCreatePort");
    Info info=(Info)resolve("sceVoiceGetPortInfo");
    SetVolume set=(SetVolume)resolve("sceVoiceSetVolume");
    GetVolume get=(GetVolume)runtime_services_resolve("jjkCjneOYSs#arbitrary#tags"); assert(get);
    struct { int32_t port; uint32_t after; } output={-1,0x12345678};
    PortParam param={.type=1,.threshold=100,.volume=0.75f};
    PortParam original=param;
    assert(create(&output.port,&param)==(int32_t)0x804E0801);
    assert(init(NULL,0)==(int32_t)0x804E0805);
    unsigned char init_param[40]={0};
    assert(!init(init_param,0));
    assert(init(init_param,0)==(int32_t)0x804E0802);
    assert(!create(&output.port,&param));
    assert(output.port>=0 && output.port<64 && output.after==0x12345678);
    assert(!memcmp(&param,&original,sizeof(param))); /* output/param argument order */
    uint32_t edges[64]; memset(edges,0x6d,sizeof(edges));
    struct { uint64_t before; PortInfo info; uint64_t after; } guard;
    memset(&guard,0xa5,sizeof(guard)); guard.info.edges=edges;
    assert(!info((uint32_t)output.port,&guard.info));
    assert(guard.info.type==1 && guard.info.state==0 && !guard.info.bytes && !guard.info.edge_count);
    assert(guard.info.edges==edges && guard.info.frame_size==1);
    unchanged(edges,sizeof(edges),0x6d);
    assert(guard.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && guard.after==guard.before); /* exactly32 bytes */
    float volume=-1;
    assert(!get((uint32_t)output.port,&volume) && volume==0.75f);
    assert(!set((uint32_t)output.port,0.125f));
    assert(!get((uint32_t)output.port,&volume) && volume==0.125f);
    uint32_t nan_bits=0x7fc00000; float nan; memcpy(&nan,&nan_bits,sizeof(nan));
    assert(!set((uint32_t)output.port,nan));
    assert(!get((uint32_t)output.port,&volume) && volume==0.0f);
    assert(set(64,1.0f)==(int32_t)0x804E0804);
    param.type=0; int32_t no_device=-1;
    assert(!create(&no_device,&param));
    assert(!info((uint32_t)no_device,&guard.info) && guard.info.type==0 && !guard.info.state && !guard.info.bytes);
    param.type=3; int32_t out_port=-1; assert(!create(&out_port,&param));
    unsigned char audio[16]; memset(audio,0x7c,sizeof(audio)); uint32_t bytes=sizeof(audio);
    assert(!((Read)resolve("sceVoiceReadFromOPort"))((uint32_t)out_port,audio,&bytes) && !bytes);
    unchanged(audio,sizeof(audio),0x7c);
    bytes=sizeof(audio);
    assert(((Write)resolve("sceVoiceWriteToIPort"))((uint32_t)output.port,audio,&bytes,0)==(int32_t)0x804E0807 && !bytes);
    assert(!((PortFn)resolve("sceVoiceDeletePort"))((uint32_t)output.port));
    assert(set((uint32_t)output.port,1.0f)==(int32_t)0x804E0804);
    assert(!call0("sceVoiceEnd"));
    assert(call0("sceVoiceEnd")== (int32_t)0x804E0801);
    assert(((PortFn)resolve("sceAudioInGetSilentState"))(0)==(int32_t)0x80260101);
    assert(((PortFn)resolve("sceAudioInGetSilentState"))(0x30000000)==(int32_t)0x80260109);

    typedef struct {
        int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label,input_method;
        void *filter; uint32_t option,max_length; uint16_t *buffer;
        float x,y; uint32_t halign,valign; const uint16_t *placeholder,*title; int8_t reserved[16];
    } Ime;
    uint16_t text[17]={0},before[17]={0};
    Ime ime={.user=1,.max_length=16,.buffer=text};
    typedef int32_t (ABI *ImeInit)(const Ime *,const void *);
    unsetenv("BB_IME_TEXT");
    assert(!((ImeInit)resolve("sceImeDialogInit"))(&ime,NULL));
    assert(call0("sceImeDialogGetStatus")==2);
    uint32_t result=99;
    assert(!callp("sceImeDialogGetResult",&result) && result==1);
    assert(!memcmp(text,before,sizeof(text))); /* unsupported UI must not type "Player" */
    assert(!call0("sceImeDialogTerm"));
    assert(!call0("sceSystemServiceDisableMusicPlayer"));
    assert(!call0("sceSystemServiceDisableMusicPlayer"));
    assert(!call0("sceSystemServiceReenableMusicPlayer"));
    assert(!call0("sceSystemServicePowerTick"));
}
static int cleanup_entry(const char *path,const struct stat *info,int kind,struct FTW *walk) {
    (void)info; (void)kind; (void)walk; return remove(path);
}
static char temporary_root[1024];
static void cleanup(void) { if (*temporary_root) nftw(temporary_root,cleanup_entry,16,FTW_DEPTH|FTW_PHYS); }
static void create_file(const char *path) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0);
    assert(write(fd,"preserved",9)==9); assert(!close(fd));
}
static void test_content(void) {
    typedef struct { char name[16]; } Mount;
    typedef int32_t (ABI *MountAddon)(uint32_t,const void *,void *);
    typedef int32_t (ABI *Space)(const Mount *,uint64_t *);
    typedef int32_t (ABI *ContentInit)(const void *,void *);
    typedef int32_t (ABI *ModuleLoad)(uint16_t);
    MountAddon mount=(MountAddon)resolve("sceAppContentAddcontMount");
    Space space=(Space)resolve("sceAppContentDownloadDataGetAvailableSpaceKb");
    Mount point={{"/download0"}}, output;
    unsigned char entitlement[20]={0};
    memset(&output,0x5a,sizeof(output));
    assert(mount(0,entitlement,&output)==(int32_t)0x80d90001);
    unchanged(&output,sizeof(output),0x5a);
    const uint32_t values[]={3,0,0,0,0}; runtime_content_configure(values);
    char template[]="/tmp/driveclub-services-XXXXXX";
    const char *root=mkdtemp(template); assert(root);
    snprintf(temporary_root,sizeof(temporary_root),"%s",root); atexit(cleanup);
    runtime_file_configure(root,root);
    ModuleLoad load=(ModuleLoad)resolve("sceSysmoduleLoadModule");
    assert(!load(0xb4));
    unsigned char init[32]={0},boot[40]; memset(boot,0x5a,sizeof(boot));
    ContentInit initialize=(ContentInit)resolve("sceAppContentInitialize");
    init[0]=1; assert(initialize(init,boot)==(int32_t)0x80d90002); unchanged(boot,sizeof(boot),0x5a);
    init[0]=0; assert(!initialize(init,boot)); unchanged(boot,sizeof(boot),0);
    assert(initialize(init,boot)==(int32_t)0x80d90003);
    assert(mount(8,entitlement,&output)==(int32_t)0x80d90002);
    assert(mount(0,NULL,&output)==(int32_t)0x80d90002);
    assert(mount(0,entitlement,&output)==(int32_t)0x80d90005);
    unchanged(&output,sizeof(output),0x5a);
    unsigned char info[24]; memset(info,0x6b,sizeof(info));
    assert(((MountAddon)resolve("sceAppContentGetAddcontInfo"))(0,entitlement,info)==(int32_t)0x80d90007);
    unchanged(info,sizeof(info),0x6b);
    assert(callp("sceAppContentAddcontUnmount",&point)==(int32_t)0x80d90004);
    uint64_t available=UINT64_MAX;
    assert(!space(&point,&available));
    struct statvfs fs; assert(!statvfs(root,&fs));
    assert(available<=((uint64_t)fs.f_blocks*fs.f_frsize)/1024 && available!=UINT64_MAX);
    Mount wrong={{"/savedata0"}};
    available=UINT64_MAX;
    assert(space(&wrong,&available)==(int32_t)0x80d90005 && available==UINT64_MAX);
    assert(callp("sceAppContentDownloadDataFormat",&wrong)==(int32_t)0x80d90005);
    memset(&wrong,'x',sizeof(wrong));
    assert(space(&wrong,&available)==(int32_t)0x80d90002 && available==UINT64_MAX);
    assert(space(&point,NULL)==(int32_t)0x80d90002);
    char download[1200], nested[1200], file[1200], outside[1200], link[1200];
    snprintf(download,sizeof(download),"%s/download0",root);
    snprintf(nested,sizeof(nested),"%s/download0/nested",root); assert(!mkdir(nested,0700));
    snprintf(file,sizeof(file),"%s/download0/nested/partial.bin",root); create_file(file);
    snprintf(outside,sizeof(outside),"%s/savedata",root); assert(!mkdir(outside,0700));
    snprintf(file,sizeof(file),"%s/savedata/save.dat",root); create_file(file);
    snprintf(link,sizeof(link),"%s/download0/save-link",root); assert(!symlink(outside,link));
    assert(!callp("sceAppContentDownloadDataFormat",&point));
    assert(!access(download,F_OK) && access(nested,F_OK) && access(link,F_OK));
    assert(!access(file,F_OK));
    snprintf(nested,sizeof(nested),"%s/download-before-link",root);
    assert(!rename(download,nested) && !symlink(outside,download));
    assert(callp("sceAppContentDownloadDataFormat",&point)!=0 && !access(file,F_OK));
    assert(!unlink(download) && !rename(nested,download));
    assert(!callp("sceAppContentDownloadDataFormat",&point));
}
static void test_rtc(void) {
    typedef int32_t (ABI *Parse)(uint64_t *,const char *);
    Parse parse=(Parse)resolve("sceRtcParseRFC3339");
    const uint64_t epoch=UINT64_C(62135596800000000);
    uint64_t tick=17;
    assert(!parse(&tick,"1970-01-01T00:00:00Z") && tick==epoch);
    assert(!parse(&tick,"1970-01-01t01:30:00+01:30") && tick==epoch);
    assert(!parse(&tick,"1969-12-31T22:30:00-01:30") && tick==epoch);
    assert(!parse(&tick,"1970-01-01T00:00:00.1z") && tick==epoch+100000);
    assert(!parse(&tick,"1970-01-01T00:00:00.123456789Z") && tick==epoch+123456);
    assert(!parse(&tick,"2000-02-29T00:00:00Z") && tick==epoch+UINT64_C(951782400000000));
    assert(!parse(&tick,"0001-01-01T00:00:00Z") && tick==0);
    const char *bad[]={"","2","1970-01-01","1970-01-01T00:00:00","1970-01-01T00:00:00.Z",
        "1970-01-01T00:00:00+24:00","1970-01-01T00:00:00+01:60","1970-01-01T00:00:00Ztail",
        "1970-01-01T00:00:60Z","1900-02-29T00:00:00Z","1970-00-01T00:00:00Z",
        "0001-01-01T00:00:00+01:00","9999-12-31T23:59:59-01:00"};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) { tick=17; assert(parse(&tick,bad[i])!=0 && tick==17); }
    assert(parse(NULL,"1970-01-01T00:00:00Z")== (int32_t)0x80B50002);
    assert(parse(&tick,NULL)== (int32_t)0x80B50002);
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    char *guard=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0); assert(guard!=MAP_FAILED);
    assert(!mprotect(guard+page,page,PROT_NONE));
    guard[page-1]=0;
    assert(parse(&tick,guard+page-1)== (int32_t)0x80B50007);
    assert(!munmap(guard,2*page));
}
int main(void) {
    unsetenv("BB_ONLINE");
    test_messages(); test_save_dialogs(); test_profile_and_error(); test_voice_and_unavailable_input();
    test_content(); test_rtc();
    puts("PASS: dialog lifecycles/results, private AppContent filesystem, RTC parsing and real NID lookup");
    return 0;
}
