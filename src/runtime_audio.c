/* libSceAudioOut on SDL3 audio streams. sceAudioOutOutput returns once per
 * buffer period on a steady clock, like PS4 hardware: FMOD's output thread
 * reads 256-frame slots out of its mixer's 512-frame ring, and returning in
 * bursts (whenever the device pulls a quantum) lets it overtake the mixer and
 * play stale halves - a click every 10.7 ms. The lowest SDL queue level of
 * each window is steered to two buffers (silence prefill on underrun, ±3%
 * cadence corrections), which also absorbs device clock drift. Without an audio device (headless runs, BB_AUDIO=none)
 * ports only follow the clock. */
#define _GNU_SOURCE
#include "runtime.h"
#include "../gpu/aegis_runtime_stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <math.h>
#include <SDL3/SDL.h>

#define PORTS 25
#define ERR_NOT_OPENED ((int32_t)0x80260001)
#define ERR_INVALID_PORT ((int32_t)0x80260003)
#define ERR_INVALID_POINTER ((int32_t)0x80260004)
#define ERR_PORT_FULL ((int32_t)0x80260005)
#define ERR_INVALID_SIZE ((int32_t)0x80260006)
#define ERR_INVALID_FORMAT ((int32_t)0x80260007)
#define ERR_INVALID_FREQ ((int32_t)0x80260008)
#define ERR_INVALID_VOLUME ((int32_t)0x80260009)
#define ERR_INVALID_TYPE ((int32_t)0x8026000A)
#define ERR_ALREADY_INIT ((int32_t)0x8026000E)
#define ERR_NOT_INIT ((int32_t)0x8026000F)
#define VOLUME_0DB 32768

typedef struct {
    int used, type, channels, is_float, frames, sample_bytes, std_layout;
    int downmix; /* 8ch guest -> stereo host; guest format and cadence stay unchanged */
    int32_t volume[8];
    SDL_AudioStream *stream;
    uint64_t next_deadline_ns; /* next return of sceAudioOutOutput */
    int64_t adjust_ns; int window_min, window_count; /* queue level control */
    uint64_t last_output_us;
    pthread_mutex_t lock;
    FILE *dump;                     /* BB_AUDIO_DUMP: raw converted PCM per port */
    int stats;
    uint64_t stat_start_ns, stat_last_ns, stat_max_gap_ns; /* BB_AUDIO_STATS */
    unsigned stat_starved, stat_buffers; int stat_min_queued;
} Port;
typedef struct { uint16_t output; uint8_t channel, reserved; int16_t volume; uint16_t reroute; uint64_t flag, reserved64[2]; } PortState;
_Static_assert(sizeof(PortState)==32,"AudioOut port state layout");

static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
static Port ports[PORTS];
static int initialized, sdl_ready=-1;
static _Atomic size_t buffers_out, ports_opened, non_silent_buffers, queue_failures;
static _Atomic unsigned debug_gain_milli=1000;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static void sleep_until(uint64_t deadline) {
    /* winpthreads rejects CLOCK_MONOTONIC in clock_nanosleep. Retrying EINVAL
     * forever stalls the second audio buffer while the game waits for voice.
     * Keep monotonic deadlines, but pass relative durations to SDL's timer. */
    for (;;) {
        uint64_t now=now_ns();
        if (now>=deadline) return;
        SDL_DelayPrecise(deadline-now);
    }
}
static int sdl_audio(void) {
    if (sdl_ready<0) {
        const char *mode=getenv("BB_AUDIO");
        sdl_ready = (!mode || strcmp(mode,"none")) && SDL_InitSubSystem(SDL_INIT_AUDIO);
        printf("Runtime: audio backend %s\n", sdl_ready ? SDL_GetCurrentAudioDriver() : "timer (silent)");
    }
    return sdl_ready;
}
/* Host master volume is separate from the PS4 game's per-channel volume. SDL applies
 * it after our speaker mix while converting/draining the stream. */
static int configure_stream_gain(SDL_AudioStream *stream) {
    float gain=1.0f, candidate;
    int mix_version=0;
    const char *path=getenv("BB_CONFIG");
    FILE *config=path ? fopen(path,"r") : NULL;
    if (config) {
        char line[512];
        while (fgets(line,sizeof(line),config)) {
            (void)sscanf(line," audio_mix_version = %d",&mix_version);
            if (sscanf(line," audio_gain = %f",&candidate)==1 &&
                isfinite(candidate) && candidate>=0.0f && candidate<=4.0f) gain=candidate;
        }
        fclose(config);
    }
    /* Old profiles used 2x to compensate for SDL's attenuated 7.1 matrix.
     * Only migrate that old default; preserve mute and other custom levels. */
    if (mix_version<1 && gain==2.0f) gain=1.0f;
    const char *override=getenv("BB_AUDIO_GAIN");
    if (override && *override) {
        char *end=NULL;
        candidate=strtof(override,&end);
        if (end!=override && !*end && isfinite(candidate) && candidate>=0.0f && candidate<=4.0f) gain=candidate;
        else fprintf(stderr,"Runtime: ignoring invalid BB_AUDIO_GAIN (expected 0..4)\n");
    }
    if (!SDL_SetAudioStreamGain(stream,gain)) return 0;
    atomic_store_explicit(&debug_gain_milli,(unsigned)(gain*1000.0f+0.5f),memory_order_relaxed);
    printf("Runtime: audio master gain %.2fx (%.0f%%)\n",gain,gain*100.0f);
    return 1;
}
static int port_range(int type, int *first, int *last) {
    switch (type) {
    case 0: *first=0; *last=7; return 1;      /* main */
    case 1: *first=8; *last=8; return 1;      /* bgm */
    case 2: *first=9; *last=12; return 1;     /* voice */
    case 3: *first=13; *last=16; return 1;    /* personal */
    case 4: *first=17; *last=20; return 1;    /* pad speaker */
    case 126: *first=21; *last=22; return 1;  /* audio3d */
    case 127: *first=23; *last=24; return 1;  /* aux */
    default: return 0;
    }
}
static Port *port_of(int32_t handle, int32_t *error) {
    int id=handle & 0xff;
    if ((handle & 0x3f000000)!=0x20000000 || id>=PORTS) { *error=ERR_INVALID_PORT; return NULL; }
    if (!ports[id].used) { *error=ERR_NOT_OPENED; return NULL; }
    return &ports[id];
}

static ABI int32_t audio_init(void) {
    pthread_mutex_lock(&table_lock);
    int32_t r=initialized ? ERR_ALREADY_INIT : 0;
    initialized=1;
    pthread_mutex_unlock(&table_lock);
    return r;
}
static ABI int32_t audio_open(int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param) {
    (void)user; (void)index;
    if (!initialized) return ERR_NOT_INIT;
    if (!length || length>2048 || (length & 0xff)) return ERR_INVALID_SIZE;
    if (freq!=48000) return ERR_INVALID_FREQ;
    uint32_t format=param & 0xff;
    if (format>7) return ERR_INVALID_FORMAT;
    int first, last;
    if (!port_range(type,&first,&last)) return ERR_INVALID_TYPE;
    static const int channels[8]={1,2,8,1,2,8,8,8};
    pthread_mutex_lock(&table_lock);
    int id=-1;
    for (int i=first;i<=last;++i) if (!ports[i].used) { id=i; break; }
    if (id<0) { pthread_mutex_unlock(&table_lock); return ERR_PORT_FULL; }
    Port *p=&ports[id];
    memset(p,0,sizeof(*p));
    pthread_mutex_init(&p->lock,NULL);
    p->used=1; p->type=type; p->channels=channels[format]; p->is_float=format>=3 && format!=6;
    p->sample_bytes=p->is_float ? 4 : 2; p->frames=(int)length; p->std_layout=format>=6;
    for (int c=0;c<8;++c) p->volume[c]=VOLUME_0DB;
    if (sdl_audio()) {
        SDL_AudioSpec spec={p->is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, p->channels, 48000};
        p->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
        if (!p->stream) {
            fprintf(stderr,"Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n",SDL_GetError(),id);
        } else {
            SDL_AudioSpec device;
            int device_frames=0;
            if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(p->stream),&device,&device_frames)) {
                printf("Runtime: audio device %d ch, %d Hz, %d frames\n",device.channels,device.freq,device_frames);
                const char *mode=getenv("BB_AUDIO_DOWNMIX");
                if (p->channels==8 && device.channels<6 && !(mode && !strcmp(mode,"0"))) {
                    /* Keep FL/FR at unity, instead of SDL's ~0.212 stereo coefficient.
                     * Based on unicorn_overlord_pc (88404b6), runtime_audio.c. */
                    spec.channels=2;
                    if (SDL_SetAudioStreamFormat(p->stream,&spec,NULL)) p->downmix=1;
                    else fprintf(stderr,"Runtime: stereo mix setup failed (%s); retaining SDL conversion\n",SDL_GetError());
                }
            } else fprintf(stderr,"Runtime: audio device format query failed (%s); retaining SDL conversion\n",SDL_GetError());
        }
        if (p->stream && (!configure_stream_gain(p->stream) || !SDL_ResumeAudioStreamDevice(p->stream))) {
            fprintf(stderr,"Runtime: SDL audio start failed (%s); port %d uses the timer sink\n",SDL_GetError(),id);
            SDL_DestroyAudioStream(p->stream);
            p->stream=NULL;
        }
    }
    p->stats=getenv("BB_AUDIO_STATS")!=NULL;
    const char *dump=getenv("BB_AUDIO_DUMP");
    if (dump) {
        char path[4096];
        snprintf(path,sizeof(path),"%s.port%d.%dch.%s",dump,id,p->downmix ? 2 : p->channels,p->is_float ? "f32" : "s16");
        p->dump=fopen(path,"wb");
    }
    ++ports_opened;
    pthread_mutex_unlock(&table_lock);
    printf("Runtime: audio port %d opened (type %d, %d ch, %s, %u frames%s)\n",id,type,p->channels,p->is_float ? "float" : "s16",length,p->downmix ? ", mixed to stereo" : "");
    return (type<<16) | id | 0x20000000;
}
static ABI int32_t audio_close(int32_t handle) {
    pthread_mutex_lock(&table_lock);
    int32_t error=0;
    Port *p=port_of(handle,&error);
    if (p) {
        pthread_mutex_lock(&p->lock);
        if (p->stream) SDL_DestroyAudioStream(p->stream);
        if (p->dump) fclose(p->dump);
        p->dump=NULL;
        p->stream=NULL; p->used=0;
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_unlock(&table_lock);
    return p ? 0 : error;
}
/* In-place 7.1 (SDL FL FR C LFE BL BR SL SR) -> stereo, after guest channel volume.
 * Dialogue/center and both surround pairs contribute at -3 dB; LFE is excluded.
 * Saturation prevents integer wrap and bounds float PCM; no whole-mix attenuation.
 * Queue sizes and diagnostic PCM describe this output, not the 8ch guest buffer. */
static size_t mix_stereo(const Port *p, unsigned char *pcm) {
    for (size_t f=0;f<(size_t)p->frames;++f) {
        float s[8];
        for (size_t c=0;c<8;++c) {
            if (p->is_float) { memcpy(&s[c],pcm+(f*8+c)*4,4); if (!isfinite(s[c])) s[c]=0; }
            else { int16_t v; memcpy(&v,pcm+(f*8+c)*2,2); s[c]=(float)v/32768.0f; }
        }
        float out[2]={s[0]+0.70710678f*(s[2]+s[4]+s[6]),
                      s[1]+0.70710678f*(s[2]+s[5]+s[7])};
        for (size_t c=0;c<2;++c) {
            float v=out[c]>1 ? 1 : out[c]<-1 ? -1 : out[c];
            if (p->is_float) memcpy(pcm+(f*2+c)*4,&v,4);
            else { int sample=(int)(v*32768.0f); int16_t value=(int16_t)(sample>32767 ? 32767 : sample); memcpy(pcm+(f*2+c)*2,&value,2); }
        }
    }
    return (size_t)p->frames*2*(size_t)p->sample_bytes;
}
/* `pace`: wait for this port's next period. sceAudioOutOutputs waits once for all its ports. */
static void queue_audio(Port *p, const void *data, int bytes) {
    if (!SDL_PutAudioStreamData(p->stream,data,bytes) &&
        atomic_fetch_add(&queue_failures,1)==0)
        fprintf(stderr,"Runtime: SDL audio queue failed: %s\n",SDL_GetError());
}
static int32_t output_port(int32_t handle, const void *data, int pace) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    pthread_mutex_lock(&p->lock);
    size_t samples=(size_t)p->frames*(size_t)p->channels, bytes=samples*(size_t)p->sample_bytes;
    uint64_t period=(uint64_t)p->frames*1000000000u/48000u;
    if (data) {
        /* Apply per-channel volume and map PS4 8ch (L R C LFE SL SR BL BR) to SDL 7.1 order. */
        unsigned char converted[2048*8*4];
        static const int remap[8]={0,1,2,3,6,7,4,5};
        int non_silent=0;
        for (size_t f=0;f<(size_t)p->frames;++f) for (int c=0;c<p->channels;++c) {
            int target=p->channels==8 && !p->std_layout ? remap[c] : c;
            size_t from=f*(size_t)p->channels+(size_t)c, to=f*(size_t)p->channels+(size_t)target;
            float gain=(float)p->volume[c]/VOLUME_0DB;
            if (p->is_float) { float v; memcpy(&v,(const char *)data+from*4,4); v*=gain; non_silent|=v!=0.0f; memcpy(converted+to*4,&v,4); }
            else { int16_t v; memcpy(&v,(const char *)data+from*2,2); v=(int16_t)((float)v*gain); non_silent|=v!=0; memcpy(converted+to*2,&v,2); }
        }
        if (p->downmix) bytes=mix_stereo(p,converted);
        if (non_silent) ++non_silent_buffers;
        if (p->dump) fwrite(converted,1,bytes,p->dump);
        if (p->stats) {
            uint64_t now=now_ns();
            if (!p->stat_start_ns) { p->stat_start_ns=now; p->stat_min_queued=1<<30; }
            else if (now-p->stat_last_ns>p->stat_max_gap_ns) p->stat_max_gap_ns=now-p->stat_last_ns;
            p->stat_last_ns=now; ++p->stat_buffers;
            if (p->stream) {
                int queued=SDL_GetAudioStreamQueued(p->stream);
                if (queued<p->stat_min_queued) p->stat_min_queued=queued;
                if (queued==0) ++p->stat_starved;
            }
            if (now-p->stat_start_ns>=5000000000u) {
                printf("Audio stats port %d: %u buffers/5s (expected %u), max gap %.2f ms, min queued %d bytes, starved %u\n",
                       (int)(p-ports),p->stat_buffers,(unsigned)(5u*48000u/(unsigned)p->frames),p->stat_max_gap_ns/1e6,
                       p->stat_min_queued,p->stat_starved);
                p->stat_start_ns=now; p->stat_max_gap_ns=0; p->stat_starved=0; p->stat_buffers=0; p->stat_min_queued=1<<30;
            }
        }
        if (pace) {
            uint64_t now=now_ns();
            if (!p->next_deadline_ns || now>p->next_deadline_ns+8*period) p->next_deadline_ns=now; /* start or stall: restart cadence */
            else sleep_until(p->next_deadline_ns);
            p->next_deadline_ns+=period;
        }
        if (p->stream) {
            /* The device drains the queue in quanta (21 ms on PipeWire), so the level
               is a sawtooth: its minimum over ~32 buffers is what gets controlled. */
            int low=2*(int)bytes, queued=SDL_GetAudioStreamQueued(p->stream);
            if (queued<(int)bytes) {
                static const unsigned char silence[2048*8*4];
                queue_audio(p,silence,low-queued<(int)bytes ? low-queued : (int)bytes);
                queued=low;
            }
            if (!p->window_count || queued<p->window_min) p->window_min=queued;
            if (++p->window_count==32) {
                p->adjust_ns=p->window_min>low+2*(int)bytes ? (int64_t)(period/32) : p->window_min<low ? -(int64_t)(period/32) : 0;
                p->window_count=0;
            }
            p->next_deadline_ns+=(uint64_t)p->adjust_ns;
            queue_audio(p,converted,(int)bytes);
        }
        ++buffers_out;
    }
    p->last_output_us=now_ns()/1000;
    pthread_mutex_unlock(&p->lock);
    return data ? (int32_t)samples : 0;
}
static ABI int32_t audio_output(int32_t handle, const void *data) { return output_port(handle,data,1); }
typedef struct { int32_t handle; const void *data; } OutputParam;
static ABI int32_t audio_outputs(const OutputParam *params, uint32_t count) {
    if (!params) return ERR_INVALID_POINTER;
    if (!count || count>PORTS) return ERR_PORT_FULL;
    int32_t result=0;
    /* One period per call for all ports (FMOD feeds main and BGM together): pacing every
       port would halve the rate and starve both. */
    for (uint32_t i=0;i<count;++i) { int32_t r=output_port(params[i].handle,params[i].data,i==0); if (r<0) return r; result=r; }
    return result;
}
static ABI int32_t audio_volume(int32_t handle, int32_t flags, const int32_t *volume) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!volume) return ERR_INVALID_POINTER;
    pthread_mutex_lock(&p->lock);
    for (int c=0;c<8;++c) if (flags & (1<<c)) {
        if (volume[c]<0 || volume[c]>VOLUME_0DB) { pthread_mutex_unlock(&p->lock); return ERR_INVALID_VOLUME; }
        p->volume[c]=volume[c];
    }
    pthread_mutex_unlock(&p->lock);
    return 0;
}
static ABI int32_t audio_state(int32_t handle, PortState *state) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!state) return ERR_INVALID_POINTER;
    memset(state,0,sizeof(*state));
    switch (p->type) {
    case 2: case 3: state->output=0x40; state->channel=1; break;          /* headphone */
    case 4: state->output=0x04; state->channel=1; state->volume=127; break; /* pad speaker */
    default: state->output=0x01; state->channel=(uint8_t)(p->channels>2 ? 2 : p->channels); break;
    }
    return 0;
}
static ABI int32_t audio_last_time(int32_t handle, uint64_t *time) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!time) return ERR_INVALID_POINTER;
    *time=p->last_output_us; return 0;
}

static const RuntimeExport exports[]={
    {"sceAudioOutInit",audio_init}, {"sceAudioOutOpen",audio_open}, {"sceAudioOutClose",audio_close},
    {"sceAudioOutOutput",audio_output}, {"sceAudioOutOutputs",audio_outputs},
    {"sceAudioOutSetVolume",audio_volume}, {"sceAudioOutGetPortState",audio_state},
    {"sceAudioOutGetLastOutputTime",audio_last_time},
};
uintptr_t runtime_audio_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_audio_report(void) {
    printf("Runtime: audio ports opened=%zu, buffers output=%zu\n",ports_opened,buffers_out);
    printf("Runtime: audio non-silent buffers=%zu, queue failures=%zu\n",non_silent_buffers,queue_failures);
}
void runtime_audio_debug(AegisRuntimeStats *stats) {
    stats->audio_buffers=atomic_load_explicit(&buffers_out,memory_order_relaxed);
    stats->audio_nonzero=atomic_load_explicit(&non_silent_buffers,memory_order_relaxed);
    stats->audio_errors=atomic_load_explicit(&queue_failures,memory_order_relaxed);
    stats->audio_ports=atomic_load_explicit(&ports_opened,memory_order_relaxed);
    stats->audio_gain_milli=atomic_load_explicit(&debug_gain_milli,memory_order_relaxed);
}
