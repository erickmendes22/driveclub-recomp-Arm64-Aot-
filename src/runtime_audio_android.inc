/* The small audio-stream interface used by runtime_audio.c, backed directly by
 * AAudio. Retaining the existing mixer and buffer cadence avoids a second FMOD
 * timing implementation and requires no SDLActivity or writable executable. */
#include <aaudio/AAudio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <errno.h>

enum { SDL_INIT_AUDIO=1, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK=1,
       SDL_AUDIO_S16=AAUDIO_FORMAT_PCM_I16, SDL_AUDIO_F32=AAUDIO_FORMAT_PCM_FLOAT };
typedef struct { int format, channels, freq; } SDL_AudioSpec;
typedef struct {
    AAudioStream *device;
    pthread_mutex_t mutex;
    unsigned char *queue;
    size_t capacity, head, used, frame_bytes;
    atomic_int disconnected;
} SDL_AudioStream;
static _Thread_local aaudio_result_t android_audio_error;
static const char *SDL_GetError(void) { return AAudio_convertResultToText(android_audio_error); }
static int SDL_InitSubSystem(unsigned flags) { (void)flags; return 1; }
static const char *SDL_GetCurrentAudioDriver(void) { return "AAudio"; }
static int SDL_GetAudioDeviceFormat(int device,SDL_AudioSpec *format,int *frames) {
    (void)device;
    *format=(SDL_AudioSpec){SDL_AUDIO_F32,2,48000};
    if (frames) *frames=192;
    return 1;
}
static aaudio_data_callback_result_t android_audio_pull(AAudioStream *device,void *opaque,
                                                       void *output,int32_t frames) {
    (void)device;
    SDL_AudioStream *stream=opaque;
    size_t bytes=(size_t)frames*stream->frame_bytes;
    memset(output,0,bytes);
    if (pthread_mutex_trylock(&stream->mutex)!=0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
    size_t available=stream->used<bytes ? stream->used : bytes;
    size_t first=available<stream->capacity-stream->head ? available : stream->capacity-stream->head;
    memcpy(output,stream->queue+stream->head,first);
    if (available>first) memcpy((unsigned char *)output+first,stream->queue,available-first);
    stream->head=(stream->head+available)%stream->capacity;
    stream->used-=available;
    pthread_mutex_unlock(&stream->mutex);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void android_audio_disconnected(AAudioStream *device,void *opaque,aaudio_result_t error) {
    (void)device;
    SDL_AudioStream *stream=opaque;
    atomic_store(&stream->disconnected,1);
    fprintf(stderr,"Android: AAudio output disconnected (%s)\n",AAudio_convertResultToText(error));
}
static SDL_AudioStream *SDL_OpenAudioDeviceStream(int device,const SDL_AudioSpec *spec,
                                                 void *callback,void *userdata) {
    (void)device; (void)callback; (void)userdata;
    SDL_AudioStream *stream=calloc(1,sizeof(*stream));
    if (!stream) { android_audio_error=AAUDIO_ERROR_NO_MEMORY; return NULL; }
    stream->frame_bytes=(size_t)spec->channels*(spec->format==SDL_AUDIO_F32 ? 4u : 2u);
    stream->capacity=(size_t)spec->freq*stream->frame_bytes*2u;
    stream->queue=malloc(stream->capacity);
    if (!stream->queue) { free(stream); android_audio_error=AAUDIO_ERROR_NO_MEMORY; return NULL; }
    pthread_mutex_init(&stream->mutex,NULL);
    AAudioStreamBuilder *builder=NULL;
    android_audio_error=AAudio_createStreamBuilder(&builder);
    if (android_audio_error==AAUDIO_OK) {
        AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSampleRate(builder,spec->freq);
        AAudioStreamBuilder_setChannelCount(builder,spec->channels);
        AAudioStreamBuilder_setFormat(builder,(aaudio_format_t)spec->format);
        AAudioStreamBuilder_setUsage(builder,AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(builder,AAUDIO_CONTENT_TYPE_MUSIC);
        AAudioStreamBuilder_setDataCallback(builder,android_audio_pull,stream);
        AAudioStreamBuilder_setErrorCallback(builder,android_audio_disconnected,stream);
        android_audio_error=AAudioStreamBuilder_openStream(builder,&stream->device);
        AAudioStreamBuilder_delete(builder);
    }
    if (android_audio_error==AAUDIO_OK &&
        (AAudioStream_getChannelCount(stream->device)!=spec->channels ||
         AAudioStream_getFormat(stream->device)!=spec->format ||
         AAudioStream_getSampleRate(stream->device)!=spec->freq))
        android_audio_error=AAUDIO_ERROR_INVALID_FORMAT;
    if (android_audio_error!=AAUDIO_OK) {
        if (stream->device) AAudioStream_close(stream->device);
        pthread_mutex_destroy(&stream->mutex); free(stream->queue); free(stream); return NULL;
    }
    printf("Android: AAudio stream %d Hz, %d channels, %d frames/burst\n",spec->freq,spec->channels,
           AAudioStream_getFramesPerBurst(stream->device));
    return stream;
}
static int SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream) {
    android_audio_error=AAudioStream_requestStart(stream->device);
    if (android_audio_error!=AAUDIO_OK) {
        atomic_store(&stream->disconnected,1);
        fprintf(stderr,"Android: AAudio start failed (%s)\n",SDL_GetError());
    }
    return android_audio_error==AAUDIO_OK;
}
static void SDL_DestroyAudioStream(SDL_AudioStream *stream) {
    if (!stream) return;
    AAudioStream_requestStop(stream->device);
    AAudioStream_close(stream->device); /* waits for callbacks before freeing userdata */
    pthread_mutex_destroy(&stream->mutex);
    free(stream->queue); free(stream);
}
static int SDL_GetAudioStreamQueued(SDL_AudioStream *stream) {
    pthread_mutex_lock(&stream->mutex);
    int queued=(int)stream->used;
    pthread_mutex_unlock(&stream->mutex);
    return queued;
}
static int SDL_PutAudioStreamData(SDL_AudioStream *stream,const void *data,int length) {
    if (length<0 || (size_t)length%stream->frame_bytes || atomic_load(&stream->disconnected)) return 0;
    const unsigned char *source=data;
    size_t bytes=(size_t)length;
    if (bytes>stream->capacity) { source+=bytes-stream->capacity; bytes=stream->capacity; }
    pthread_mutex_lock(&stream->mutex);
    if (stream->used+bytes>stream->capacity) {
        size_t dropped=stream->used+bytes-stream->capacity;
        stream->head=(stream->head+dropped)%stream->capacity; stream->used-=dropped;
    }
    size_t tail=(stream->head+stream->used)%stream->capacity;
    size_t first=bytes<stream->capacity-tail ? bytes : stream->capacity-tail;
    memcpy(stream->queue+tail,source,first);
    if (bytes>first) memcpy(stream->queue,source+first,bytes-first);
    stream->used+=bytes;
    pthread_mutex_unlock(&stream->mutex);
    return 1;
}
