/* Android libScePad: MotionEvent/KeyEvent and the on-screen controls update a
 * shared snapshot. Guest threads read the same 120-byte PS4 pad ABI as desktop.
 * No SDLActivity or SDL Java sources are needed by the native renderer. */
#ifdef __ANDROID__
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_android.h"
#include "runtime_exit.h"
#include "gpu/bbgpu.h"
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define ERR_TOO_MANY_OPENED ((int32_t)0x80920007)
#define MAX_HANDLES 16
#define BTN_L2 0x100u
#define BTN_R2 0x200u
#define BTN_TOUCHPAD 0x100000u

typedef struct { uint16_t x,y; uint8_t id,reserved[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x,left_y,right_x,right_y,l2,r2,analog_padding[2];
    float orientation[4],acceleration[3],angular_velocity[3];
    uint8_t touch_count,touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected,pad0[3];
    uint64_t timestamp;
    uint8_t extension[16],connected_count,reserve[2],unique_length,unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x,resolution_y;
    uint8_t dead_zone_left,dead_zone_right,connection_type,connected_count,connected,pad[3];
    int32_t device_class; uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"PS4 pad ABI");
_Static_assert(offsetof(PadData,timestamp)==80,"PS4 pad timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"PS4 controller info ABI");

static pthread_mutex_t pad_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { bool used,ext; int32_t user,type,index; } handles[MAX_HANDLES];
static PadData current={.left_x=128,.left_y=128,.right_x=128,.right_y=128,
                        .orientation={0,0,0,1},.connected=1,.connected_count=1};
static bool initialized;
static size_t reads;
static uint8_t light[3]={0,0,255};
static uint64_t now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
static uint8_t axis(float value) {
    if (!isfinite(value)) value=0;
    if (value<=-1) return 0;
    if (value>=1) return 255;
    return (uint8_t)lroundf(128.0f+value*(value<0 ? 128.0f : 127.0f));
}
static uint8_t trigger(float value) {
    if (!isfinite(value) || value<=0) return 0;
    return value>=1 ? 255 : (uint8_t)lroundf(value*255.0f);
}
void runtime_android_pad_update(uint32_t buttons,float lx,float ly,float rx,float ry,float l2,float r2) {
    pthread_mutex_lock(&pad_lock);
    current.buttons=buttons;
    current.left_x=axis(lx); current.left_y=axis(ly);
    current.right_x=axis(rx); current.right_y=axis(ry);
    current.l2=trigger(l2); current.r2=trigger(r2);
    if (current.l2>30) current.buttons|=BTN_L2;
    if (current.r2>30) current.buttons|=BTN_R2;
    current.touch_count=(buttons&BTN_TOUCHPAD) ? 1 : 0;
    current.touches[0]=(PadTouch){.x=480,.y=471,.id=0};
    pthread_mutex_unlock(&pad_lock);
}
static bool valid(int32_t handle) { return handle>=1 && handle<=MAX_HANDLES && handles[handle-1].used; }
static ABI int32_t pad_init(void) {
    pthread_mutex_lock(&pad_lock); initialized=true; pthread_mutex_unlock(&pad_lock); return 0;
}
static int32_t open_port(int32_t user,int32_t type,int32_t index,bool ext) {
    pthread_mutex_lock(&pad_lock);
    if (!initialized) { pthread_mutex_unlock(&pad_lock); return ERR_NOT_INITIALIZED; }
    if (user<0 || type==1) { pthread_mutex_unlock(&pad_lock); return ERR_INVALID_ARG; }
    int available=-1;
    for (int i=0;i<MAX_HANDLES;++i) {
        if (!handles[i].used) { if (available<0) available=i; continue; }
        if (handles[i].user==user && handles[i].type==type && handles[i].index==index && handles[i].ext==ext) {
            pthread_mutex_unlock(&pad_lock); return ERR_ALREADY_OPENED;
        }
    }
    if (available<0) { pthread_mutex_unlock(&pad_lock); return ERR_TOO_MANY_OPENED; }
    handles[available].used=true; handles[available].ext=ext;
    handles[available].user=user; handles[available].type=type; handles[available].index=index;
    pthread_mutex_unlock(&pad_lock);
    printf("Android: pad %d opened (user %d, type %d, index %d%s)\n",available+1,user,type,index,ext ? ", ext" : "");
    return available+1;
}
static ABI int32_t pad_open(int32_t user,int32_t type,int32_t index,const void *param) {
    (void)param; return open_port(user,type,index,false);
}
static ABI int32_t pad_open_ext(int32_t user,int32_t type,int32_t index,const void *param) {
    (void)param; return open_port(user,type==1 ? 0 : type,index,true);
}
static ABI int32_t pad_close(int32_t handle) {
    pthread_mutex_lock(&pad_lock);
    int32_t result=valid(handle) ? 0 : ERR_INVALID_HANDLE;
    if (!result) handles[handle-1].used=false;
    pthread_mutex_unlock(&pad_lock); return result;
}
static ABI int32_t pad_read_state(int32_t handle,PadData *out) {
    if (!out) return ERR_INVALID_ARG;
    runtime_exit_tick();
    bbgpu_reflex_pre_input_sample();
    pthread_mutex_lock(&pad_lock);
    if (!valid(handle)) { pthread_mutex_unlock(&pad_lock); return ERR_INVALID_HANDLE; }
    *out=current; out->timestamp=now_us(); ++reads;
    pthread_mutex_unlock(&pad_lock);
    bbgpu_reflex_on_input_sampled();
    if (bbgpu_overlay_captures_input() || runtime_exit_captures()) {
        out->buttons=0; out->left_x=out->left_y=out->right_x=out->right_y=128;
        out->l2=out->r2=out->touch_count=0;
    }
    return 0;
}
static ABI int32_t pad_read(int32_t handle,PadData *out,int32_t count) {
    if (count<1 || count>64) return ERR_INVALID_ARG;
    int32_t result=pad_read_state(handle,out); return result ? result : 1;
}
static ABI int32_t pad_info(int32_t handle,ControllerInfo *info) {
    pthread_mutex_lock(&pad_lock);
    if (!valid(handle)) { pthread_mutex_unlock(&pad_lock); return ERR_INVALID_HANDLE; }
    pthread_mutex_unlock(&pad_lock);
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connected=info->connected_count=1; /* standard gamepad; no wheel is advertised */
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) {
    pthread_mutex_lock(&pad_lock); int32_t result=valid(handle) ? 0 : ERR_INVALID_HANDLE;
    pthread_mutex_unlock(&pad_lock); return result;
}
static ABI int32_t pad_ok_flag(int32_t handle,uint8_t flag) { (void)flag; return pad_ok_handle(handle); }
static ABI int32_t pad_ext_info(int32_t handle,void *info) {
    (void)info; return pad_ok_handle(handle) ? ERR_INVALID_HANDLE : ERR_INVALID_ARG;
}
static ABI int32_t pad_device_info(int32_t handle,void *info) {
    int32_t result=pad_ok_handle(handle); if (result) return result;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,20); return 0; /* ScePadDeviceClassExtendedInformation, class standard */
}
static ABI int32_t pad_device_parse(int32_t handle,const PadData *data,void *info) {
    int32_t result=pad_ok_handle(handle); if (result) return result;
    if (!info || !data) return ERR_INVALID_ARG;
    memset(info,0,24); return 0; /* ScePadDeviceClassData, no wheel extension */
}
static ABI int32_t pad_vibration(int32_t handle,const uint8_t *motors) {
    int32_t result=pad_ok_handle(handle); if (result) return result;
    if (!motors) return ERR_INVALID_ARG;
    static int reported;
    if ((motors[0] || motors[1]) && !__atomic_exchange_n(&reported,1,__ATOMIC_RELAXED))
        puts("Android: controller vibration is unavailable in this input backend.");
    return 0;
}
static ABI int32_t pad_light(int32_t handle,const uint8_t *rgb) {
    int32_t result=pad_ok_handle(handle); if (result) return result;
    if (!rgb) return ERR_INVALID_ARG;
    pthread_mutex_lock(&pad_lock); memcpy(light,rgb,3); pthread_mutex_unlock(&pad_lock); return 0;
}
static ABI int32_t pad_reset_light(int32_t handle) { const uint8_t rgb[3]={0,0,255}; return pad_light(handle,rgb); }
static const RuntimeExport exports[]={
    {"scePadInit",pad_init},{"scePadOpen",pad_open},{"scePadOpenExt",pad_open_ext},{"scePadClose",pad_close},
    {"scePadReadState",pad_read_state},{"scePadRead",pad_read},{"scePadGetControllerInformation",pad_info},
    {"scePadGetExtControllerInformation",pad_ext_info},{"scePadDeviceClassGetExtendedInformation",pad_device_info},
    {"scePadDeviceClassParseData",pad_device_parse},{"scePadSetVibration",pad_vibration},
    {"scePadSetLightBar",pad_light},{"scePadSetLightBarForTracker",pad_light},{"scePadResetLightBar",pad_reset_light},
    {"scePadResetOrientation",pad_ok_handle},{"scePadSetLightBarBaseBrightness",pad_ok_flag},
    {"scePadSetLightBarBlinking",pad_ok_flag},{"scePadSetVibrationForce",pad_ok_flag},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_flag},{"scePadSetTiltCorrectionState",pad_ok_flag},
    {"scePadSetMotionSensorState",pad_ok_flag}
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, input=Android, wheel=none\n",reads); }
int runtime_pad_pedals(float *throttle,float *brake,float *clutch) {
    pthread_mutex_lock(&pad_lock);
    *throttle=current.r2/255.0f; *brake=current.l2/255.0f; *clutch=0;
    pthread_mutex_unlock(&pad_lock); return 1;
}
size_t runtime_input_keys_for_button(uint32_t button,char *out,size_t size) {
    (void)button; if (size) out[0]=0; return 0; /* preserve the game's controller glyphs */
}
void runtime_ffb_release(const char *why) { (void)why; /* no wheel device is opened */ }
#endif
