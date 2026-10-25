/* DirectSound 8 on a host software mixer.
 *
 * Secondary buffers live in guest memory (Lock returns pointers into them).
 * A mixer thread advances every playing buffer's play cursor in real time,
 * mixes PCM with volume, pan, frequency and a simple 3D model, fires position
 * notifications, and hands the stereo result to the audio backend (null until
 * an output is attached), so streaming code sees DirectSound's timing. */
#define _GNU_SOURCE
#include "com.h"
#include <errno.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>
#include <strings.h>
#include <unistd.h>

#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_UNSUPPORTED 0x80004001u
#define DSERR_CONTROLUNAVAIL 0x8878001Eu

#define DSBCAPS_PRIMARYBUFFER 0x1u
#define DSBCAPS_CTRL3D 0x10u
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x100u
#define DSBSTATUS_PLAYING 0x1u
#define DSBSTATUS_LOOPING 0x4u
#define DSBPLAY_LOOPING 0x1u

#define MIX_RATE 48000
#define MIX_CHUNK 480                  /* 10 ms */

static const uint8_t IID_IUnknown[16] = {0,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46};
static const uint8_t IID_IDirectSound[16] = {0x83,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60};
static const uint8_t IID_IDirectSound8[16] = {0x93,0x7E,0x0A,0xC5,0x95,0xF3,0x34,0x48,0x9E,0xF6,0x7F,0xA9,0x9D,0xE5,0x09,0x66};   /* {C50A7E93-F395-4834-9EF6-7FA99DE50966} */
static const uint8_t IID_IDirectSoundBuffer[16] = {0x85,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60};
static const uint8_t IID_IDirectSoundBuffer8[16] = {0x49,0xA4,0x25,0x68,0x24,0x75,0x82,0x4D,0x92,0x0F,0x50,0xE3,0x6A,0xB3,0xAB,0x1E};
static const uint8_t IID_IDirectSound3DBuffer[16] = {0x86,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60};
static const uint8_t IID_IDirectSound3DListener[16] = {0x84,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60};
static const uint8_t IID_IDirectSoundNotify[16] = {0x83,0x07,0x21,0xB0,0xCD,0x89,0xD0,0x11,0xAF,0x08,0x00,0xA0,0xC9,0x25,0xCD,0x16};

typedef struct dsbuf dsbuf_t;
typedef struct { void_com_object com; dsbuf_t *owner; } facet_t;

struct dsbuf {
    void_com_object com;
    facet_t f3d, notify;            /* the 3D / notify interfaces of this buffer */
    int primary;
    uint32_t flags;
    uint32_t data, size;            /* guest PCM */
    uint32_t channels, rate, bits, block;
    uint32_t frequency;
    int32_t volume, pan;
    uint32_t play_pos;              /* bytes */
    double frac;                    /* fractional source position */
    int playing, looping;
    uint32_t notify_count;
    uint32_t notify_offset[64], notify_event[64];
    uint32_t locks, last_status;    /* VOID_DSOUND_TRACE bookkeeping */
    uint32_t unlocks, reported_unlocks;
    uint64_t last_unlock_ns, max_unlock_gap_ns;
    uint8_t *fresh;
    uint32_t last_consumed, source_frames, stale_frames, stale_run, max_stale_run, playing_unlocks;
    float pos[3], min_distance, max_distance;
    uint32_t mode3d;
    dsbuf_t *next;
};

static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
static dsbuf_t *buffers;
static float listener_pos[3], listener_front[3] = {0, 0, 1}, listener_top[3] = {0, 1, 0};
static float rolloff = 1.0f, distance_factor = 1.0f;
static pthread_t mixer_thread;
static _Atomic int mixer_running;
static _Atomic uint64_t mixer_frames;
static void wave_mix(float *mix, uint32_t frames);
extern void void_restore_host_affinity(void) __attribute__((weak));

/* VOID_DSOUND_TRACE=1: log buffer creation, Play/Stop/position/frequency
 * calls, status changes and the mixer reaching the end of a buffer. */
static int dstrace;
#define DSTRACE(...) do { if (dstrace) void_log("[dsound] " __VA_ARGS__); } while (0)

/* The sink owns pacing: Android's blocking writes follow the device clock. */
__attribute__((weak)) void void_audio_output(const int16_t *frames, uint32_t count)
{
    (void)frames;
    uint64_t duration = (uint64_t)count * 1000000000u / MIX_RATE;
    struct timespec delay = { (time_t)(duration / 1000000000u), (long)(duration % 1000000000u) };
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}
__attribute__((weak)) void void_audio_wait_ready(void) {}
__attribute__((weak)) void void_audio_drain(void) {}
__attribute__((weak)) void void_audio_begin_shutdown(void) {}

static void signal_event(uint32_t handle)
{
    void_object *o = void_handle_get(handle, VOID_OBJ_EVENT);
    if (o) void_object_signal(o);
}

/* Read one sample (as float, channel c) of buffer b at byte position pos. */
static float sample_at(dsbuf_t *b, uint32_t frame, uint32_t c)
{
    uint32_t offset = frame * b->block + (c < b->channels ? c : 0) * (b->bits / 8);
    if (offset + b->bits / 8 > b->size) return 0.0f;
    if (b->bits == 16) return (float)(int16_t)MEM16(b->data + offset) / 32768.0f;
    return ((float)MEM8(b->data + offset) - 128.0f) / 128.0f;
}

static void notify_passed(dsbuf_t *b, uint32_t from, uint32_t to, int wrapped)
{
    for (uint32_t i = 0; i < b->notify_count; ++i) {
        uint32_t off = b->notify_offset[i];
        if (off == 0xFFFFFFFFu) continue;
        int hit = wrapped ? (off >= from || off < to) : (off >= from && off < to);
        if (hit) signal_event(b->notify_event[i]);
    }
}

static void mix_chunk(float *mix, uint32_t frames)
{
    memset(mix, 0, sizeof(float) * 2 * frames);
    for (dsbuf_t *b = buffers; b; b = b->next) {
        if (b->primary || !b->playing || !b->size || !b->block) continue;
        float vol = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
        float pan = (float)b->pan / 10000.0f;
        float gl = vol * (pan > 0 ? powf(10.0f, -pan * 5.0f) : 1.0f);
        float gr = vol * (pan < 0 ? powf(10.0f, pan * 5.0f) : 1.0f);
        if (b->flags & DSBCAPS_CTRL3D && b->mode3d != 2) {       /* not DS3DMODE_DISABLE */
            float d[3];
            float dist = 0.0f;
            for (int k = 0; k < 3; ++k) {
                d[k] = b->mode3d == 1 ? b->pos[k] : (b->pos[k] - listener_pos[k]);   /* HEADRELATIVE */
                dist += d[k] * d[k];
            }
            dist = sqrtf(dist) * distance_factor;
            float atten = 1.0f;
            if (dist > b->min_distance && b->min_distance > 0.0f) {
                float dd = dist > b->max_distance ? b->max_distance : dist;
                atten = b->min_distance / (b->min_distance + rolloff * (dd - b->min_distance));
            }
            /* Right vector = top x front; pan from the projection. */
            float right[3] = {
                listener_top[1] * listener_front[2] - listener_top[2] * listener_front[1],
                listener_top[2] * listener_front[0] - listener_top[0] * listener_front[2],
                listener_top[0] * listener_front[1] - listener_top[1] * listener_front[0] };
            float side = 0.0f;
            if (dist > 0.0001f) side = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (dist / distance_factor);
            gl *= atten * (side > 0 ? 1.0f - 0.5f * side : 1.0f);
            gr *= atten * (side < 0 ? 1.0f + 0.5f * side : 1.0f);
        }
        uint32_t total = b->size / b->block;
        double step = (double)b->frequency / MIX_RATE;
        uint32_t start = b->play_pos;
        double frame = (double)(b->play_pos / b->block) + b->frac;
        int wrapped = 0, stopped = 0;
        for (uint32_t i = 0; i < frames; ++i) {
            uint32_t f = (uint32_t)frame;
            if (f >= total) {
                if (!b->looping) { stopped = 1; break; }
                frame -= total; f = (uint32_t)frame; wrapped = 1;
                b->last_consumed = 0xFFFFFFFFu;
            }
            float l = sample_at(b, f, 0), r = b->channels > 1 ? sample_at(b, f, 1) : l;
            if (b->fresh && b->looping && f != b->last_consumed) {
                ++b->source_frames;
                if (!b->fresh[f]) {
                    ++b->stale_frames;
                    if (++b->stale_run > b->max_stale_run) b->max_stale_run = b->stale_run;
                } else b->stale_run = 0;
                b->fresh[f] = 0;
                b->last_consumed = f;
            }
            mix[2 * i] += l * gl;
            mix[2 * i + 1] += r * gr;
            frame += step;
        }
        /* The chunk can end exactly on the last frame (a sound whose length is
         * a whole number of 10 ms ticks): a one-shot buffer stops there; the
         * modulo below would otherwise restart it from 0 for ever. */
        if (frame >= total) {
            if (b->looping) { frame = fmod(frame, (double)total); wrapped = 1; }
            else stopped = 1;
        }
        uint32_t f = (uint32_t)frame;
        if (f > total) f = total;
        b->frac = frame - (double)(uint32_t)frame;
        b->play_pos = (f * b->block) % (b->size ? b->size : 1);
        if (stopped) {
            DSTRACE("buffer %08X reached its end (%u bytes)", b->com.guest, b->size);
            b->playing = 0;
            b->play_pos = 0;
            b->frac = 0;
            for (uint32_t k = 0; k < b->notify_count; ++k) if (b->notify_offset[k] == 0xFFFFFFFFu) signal_event(b->notify_event[k]);
        }
        notify_passed(b, start, stopped ? b->size : b->play_pos, wrapped);
    }
}

static void *mixer_main(void *arg)
{
    (void)arg;
    /* pthreads inherit the guest's big-core mask; let Android place the mixer. */
    if (void_restore_host_affinity) void_restore_host_affinity();
    float mix[2 * MIX_CHUNK];
    int16_t out[2 * MIX_CHUNK];
    uint32_t ticks = 0;
    while (mixer_running) {
        void_audio_wait_ready();
        if (!mixer_running) break;
        pthread_mutex_lock(&mix_lock);
        mix_chunk(mix, MIX_CHUNK);
        wave_mix(mix, MIX_CHUNK);
        if (++ticks % 1000 == 0)
            for (dsbuf_t *b = buffers; b; b = b->next)
                if (b->playing && b->looping && !b->primary && b->size) {
                    uint64_t now = void_monotonic_ns();
                    double age = b->last_unlock_ns ? (now - b->last_unlock_ns) / 1e6 : 0;
                    void_log("[dsound] stream %08X: refills %u, max gap %.1f ms, age %.1f ms, ring %.1f ms",
                        b->com.guest, b->unlocks - b->reported_unlocks, b->max_unlock_gap_ns / 1e6, age,
                        1000.0 * b->size / (b->block * (double)b->frequency));
                    if (b->fresh && b->playing_unlocks >= 2)
                        void_log("[dsound] producer receipt: stale %u/%u source frames, longest %.1f ms without refill",
                            b->stale_frames, b->source_frames, 1000.0 * b->max_stale_run / b->frequency);
                    b->reported_unlocks = b->unlocks; b->max_unlock_gap_ns = 0;
                    b->stale_frames = b->source_frames = b->max_stale_run = 0;
                }
        pthread_mutex_unlock(&mix_lock);
        for (int i = 0; i < 2 * MIX_CHUNK; ++i) {
            float v = mix[i] * 32767.0f;
            out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
        void_audio_output(out, MIX_CHUNK);
        atomic_fetch_add(&mixer_frames, MIX_CHUNK);
    }
    return NULL;
}

static void start_mixer(void)
{
    if (mixer_running) return;
    mixer_running = 1;
    if (pthread_create(&mixer_thread, NULL, mixer_main, NULL)) void_fatal("audio mixer thread creation failed");
}

void void_dsound_shutdown(void)
{
    void_audio_begin_shutdown();
    if (atomic_exchange(&mixer_running, 0)) pthread_join(mixer_thread, NULL);
    void_audio_drain();
}

/* Miles and Bink stream PCM through the same output as DirectSound. */
typedef struct wave_packet {
    uint32_t header, data, bytes, handle, callback, instance, flags, message;
    struct wave_packet *next;
} wave_packet;
typedef struct {
    uint32_t handle, channels, rate, bits, block, volume, callback, instance, flags;
    int active, paused;
    double frame;
    wave_packet *head, *tail;
} wave_device;
static wave_device wave_devices[8];
static uint32_t next_wave_handle = 0x71000000u;
static pthread_mutex_t wave_callback_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wave_callback_cond = PTHREAD_COND_INITIALIZER;
static wave_packet *wave_callbacks, *wave_callback_tail;
static pthread_once_t wave_callback_once = PTHREAD_ONCE_INIT;
extern void void_post_thread_message(uint32_t thread, uint32_t msg, uint32_t wparam, uint32_t lparam);

static void *wave_callback_main(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&wave_callback_lock);
        while (!wave_callbacks) pthread_cond_wait(&wave_callback_cond, &wave_callback_lock);
        wave_packet *p = wave_callbacks;
        wave_callbacks = p->next;
        if (!wave_callbacks) wave_callback_tail = NULL;
        pthread_mutex_unlock(&wave_callback_lock);
        switch (p->flags & 0x70000u) {
        case 0x10000u: void_post_message(p->callback, p->message, p->handle, p->header); break;
        case 0x20000u: void_post_thread_message(p->callback, p->message, p->handle, p->header); break;
        case 0x30000u:
            if (p->callback) {
                void_machine_enter();
                uint32_t args[5] = { p->handle, p->message, p->instance, p->header, 0 };
                void_call_guest(p->callback, 5, args, 1);
                void_machine_leave();
            }
            break;
        case 0x50000u: signal_event(p->callback); break;
        }
        free(p);
    }
    return NULL;
}

static void wave_callback_start(void)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, wave_callback_main, NULL)) void_fatal("waveOut callback thread failed");
    pthread_detach(thread);
}

static void wave_notify(wave_packet *p, uint32_t message)
{
    p->message = message; p->next = NULL;
    if (!(p->flags & 0x70000u)) { free(p); return; }
    pthread_once(&wave_callback_once, wave_callback_start);
    pthread_mutex_lock(&wave_callback_lock);
    if (wave_callback_tail) wave_callback_tail->next = p;
    else wave_callbacks = p;
    wave_callback_tail = p;
    pthread_cond_signal(&wave_callback_cond);
    pthread_mutex_unlock(&wave_callback_lock);
}

static wave_packet *wave_packet_new(wave_device *w, uint32_t header)
{
    wave_packet *p = (wave_packet *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->handle = w->handle; p->callback = w->callback; p->instance = w->instance;
    p->flags = w->flags; p->header = header;
    return p;
}

static wave_device *wave_find(uint32_t handle)
{
    for (size_t i = 0; i < sizeof wave_devices / sizeof wave_devices[0]; ++i)
        if (wave_devices[i].active && wave_devices[i].handle == handle) return &wave_devices[i];
    return NULL;
}

static void wave_finish_head(wave_device *w)
{
    wave_packet *p = w->head;
    w->head = p->next;
    if (!w->head) w->tail = NULL;
    MEM32(p->header + 16) = (MEM32(p->header + 16) | 1u) & ~0x10u; /* DONE, !INQUEUE */
    wave_notify(p, 0x3BDu); /* WOM_DONE */
}

static void wave_mix(float *mix, uint32_t frames)
{
    for (size_t k = 0; k < sizeof wave_devices / sizeof wave_devices[0]; ++k) {
        wave_device *w = &wave_devices[k];
        if (!w->active || w->paused) continue;
        for (uint32_t i = 0; i < frames && w->head; ++i) {
            while (w->head && w->frame >= w->head->bytes / w->block) {
                w->frame -= w->head->bytes / w->block;
                wave_finish_head(w);
            }
            if (!w->head) { w->frame = 0; break; }
            wave_packet *p = w->head;
            uint32_t pos = p->data + (uint32_t)w->frame * w->block;
            float l = w->bits == 16 ? (float)(int16_t)MEM16(pos) / 32768.0f : ((float)MEM8(pos) - 128) / 128.0f;
            uint32_t rp = pos + (w->channels == 2 ? w->bits / 8 : 0);
            float r = w->bits == 16 ? (float)(int16_t)MEM16(rp) / 32768.0f : ((float)MEM8(rp) - 128) / 128.0f;
            mix[2 * i] += l * (float)(w->volume & 0xFFFFu) / 65535.0f;
            mix[2 * i + 1] += r * (float)(w->volume >> 16) / 65535.0f;
            w->frame += (double)w->rate / MIX_RATE;
        }
        while (w->head && w->frame >= w->head->bytes / w->block) {
            w->frame -= w->head->bytes / w->block;
            wave_finish_head(w);
        }
        if (!w->head) w->frame = 0;
    }
}

#define WINMM "winmm.dll"
WINAPI_EXPORT(WINMM, waveOutGetNumDevs, 0) { return 1; }
WINAPI_EXPORT(WINMM, waveOutOpen, 6)
{
    uint32_t fmt = ARG(2), flags = ARG(5), type = flags & 0x70000u;
    if (ARG(1) != 0 && ARG(1) != 0xFFFFFFFFu) return 2; /* BADDEVICEID */
    if (!fmt || !void_vm_readable(fmt, 16)) return 11;
    uint32_t channels = MEM16(fmt + 2), rate = MEM32(fmt + 4), bits = MEM16(fmt + 14), block = MEM16(fmt + 12);
    if (MEM16(fmt) != 1 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16) ||
        !rate || rate > 192000 || block != channels * bits / 8) return 32; /* WAVERR_BADFORMAT */
    if (flags & 1u) return 0; /* WAVE_FORMAT_QUERY */
    if (!ARG(0) || !void_vm_writable(ARG(0), 4)) return 11;
    if (type != 0 && type != 0x10000u && type != 0x20000u && type != 0x30000u && type != 0x50000u) return 10;
    if (type == 0x50000u && !void_handle_get(ARG(3), VOID_OBJ_EVENT)) return 11;
    pthread_mutex_lock(&mix_lock);
    wave_device *w = NULL;
    for (size_t i = 0; i < sizeof wave_devices / sizeof wave_devices[0]; ++i)
        if (!wave_devices[i].active) { w = &wave_devices[i]; break; }
    if (!w) { pthread_mutex_unlock(&mix_lock); return 4; } /* ALLOCATED */
    *w = (wave_device){ .handle = next_wave_handle++, .channels = channels, .rate = rate,
        .bits = bits, .block = block, .volume = 0xFFFFFFFFu, .callback = ARG(3),
        .instance = ARG(4), .flags = flags, .active = 1 };
    wave_packet *opened = wave_packet_new(w, 0);
    if (!opened) { w->active = 0; pthread_mutex_unlock(&mix_lock); return 7; }
    MEM32(ARG(0)) = w->handle;
    wave_notify(opened, 0x3BBu); /* WOM_OPEN */
    pthread_mutex_unlock(&mix_lock);
    start_mixer();
    return 0;
}
WINAPI_EXPORT(WINMM, waveOutPrepareHeader, 3)
{
    if (!wave_find(ARG(0))) return 5;
    if (ARG(2) < 32 || !ARG(1) || !void_vm_writable(ARG(1), 32)) return 11;
    uint32_t hdr = ARG(1), bytes = MEM32(hdr + 4);
    if (!void_vm_readable(MEM32(hdr), bytes)) return 11;
    MEM32(hdr + 16) = (MEM32(hdr + 16) | 2u) & ~1u;
    return 0;
}
WINAPI_EXPORT(WINMM, waveOutUnprepareHeader, 3)
{
    if (!wave_find(ARG(0))) return 5;
    if (ARG(2) < 32 || !ARG(1) || !void_vm_writable(ARG(1), 32)) return 11;
    if (MEM32(ARG(1) + 16) & 0x10u) return 33; /* STILLPLAYING */
    MEM32(ARG(1) + 16) &= ~2u;
    return 0;
}
WINAPI_EXPORT(WINMM, waveOutWrite, 3)
{
    wave_device *w = wave_find(ARG(0));
    if (!w) return 5;
    uint32_t hdr = ARG(1);
    if (ARG(2) < 32 || !hdr || !void_vm_writable(hdr, 32)) return 11;
    uint32_t flags = MEM32(hdr + 16), bytes = MEM32(hdr + 4), data = MEM32(hdr);
    if (!(flags & 2u)) return 34; /* UNPREPARED */
    if (flags & 0x10u) return 33;
    if (flags & 0xCu) return 8; /* NOTSUPPORTED: loop headers */
    if (bytes % w->block || !void_vm_readable(data, bytes)) return 11;
    wave_packet *p = wave_packet_new(w, hdr);
    if (!p) return 7;
    p->data = data; p->bytes = bytes;
    pthread_mutex_lock(&mix_lock);
    MEM32(hdr + 16) = (flags | 0x10u) & ~1u;
    if (w->tail) w->tail->next = p;
    else w->head = p;
    w->tail = p;
    pthread_mutex_unlock(&mix_lock);
    return 0;
}
WINAPI_EXPORT(WINMM, waveOutPause, 1)
{
    wave_device *w = wave_find(ARG(0)); if (!w) return 5;
    pthread_mutex_lock(&mix_lock); w->paused = 1; pthread_mutex_unlock(&mix_lock); return 0;
}
WINAPI_EXPORT(WINMM, waveOutRestart, 1)
{
    wave_device *w = wave_find(ARG(0)); if (!w) return 5;
    pthread_mutex_lock(&mix_lock); w->paused = 0; pthread_mutex_unlock(&mix_lock); return 0;
}
WINAPI_EXPORT(WINMM, waveOutReset, 1)
{
    wave_device *w = wave_find(ARG(0)); if (!w) return 5;
    pthread_mutex_lock(&mix_lock);
    while (w->head) wave_finish_head(w);
    w->frame = 0; w->paused = 0;
    pthread_mutex_unlock(&mix_lock);
    return 0;
}
WINAPI_EXPORT(WINMM, waveOutClose, 1)
{
    wave_device *w = wave_find(ARG(0)); if (!w) return 5;
    pthread_mutex_lock(&mix_lock);
    if (w->head) { pthread_mutex_unlock(&mix_lock); return 33; }
    wave_packet *closed = wave_packet_new(w, 0);
    if (!closed) { pthread_mutex_unlock(&mix_lock); return 7; }
    w->active = 0;
    wave_notify(closed, 0x3BCu); /* WOM_CLOSE */
    pthread_mutex_unlock(&mix_lock);
    return 0;
}
WINAPI_EXPORT(WINMM, waveOutSetVolume, 2)
{
    wave_device *w = wave_find(ARG(0)); if (!w) return 5;
    pthread_mutex_lock(&mix_lock); w->volume = ARG(1); pthread_mutex_unlock(&mix_lock); return 0;
}
WINAPI_EXPORT(WINMM, waveOutGetID, 2)
{
    if (!wave_find(ARG(0))) return 5;
    if (!ARG(1)) return 11;
    MEM32(ARG(1)) = 0; return 0;
}
WINAPI_EXPORT(WINMM, waveOutGetDevCapsA, 3)
{
    if (ARG(0) && ARG(0) != 0xFFFFFFFFu && !wave_find(ARG(0))) return 2;
    if (!ARG(1) || !ARG(2)) return 11;
    uint8_t caps[52] = {0};
    uint16_t one = 1, channels = 2; uint32_t version = 0x100, formats = 0xFFFFu, support = 0xCu;
    memcpy(caps, &one, 2); memcpy(caps + 2, &one, 2); memcpy(caps + 4, &version, 4);
    strcpy((char *)caps + 8, "Stronghold Android PCM");
    memcpy(caps + 40, &formats, 4); memcpy(caps + 44, &channels, 2); memcpy(caps + 48, &support, 4);
    uint32_t n = ARG(2) < sizeof caps ? ARG(2) : sizeof caps;
    if (!void_vm_writable(ARG(1), n)) return 11;
    memcpy(GPTR(ARG(1)), caps, n); return 0;
}

/* ------------------------------------------------------------- buffers */
static void_com_class buffer_class, ds3d_class, notify_class, listener_class;

static void buffer_destroy(void_com_object *o)
{
    dsbuf_t *b = (dsbuf_t *)o;
    DSTRACE("buffer %08X released (playing %d at %u of %u)", b->com.guest, b->playing, b->play_pos, b->size);
    pthread_mutex_lock(&mix_lock);
    for (dsbuf_t **p = &buffers; *p; p = &(*p)->next) if (*p == b) { *p = b->next; break; }
    pthread_mutex_unlock(&mix_lock);
    if (b->data) void_gfree(b->data);
    free(b->fresh);
    free(b);
}

static dsbuf_t *buffer_of(uint32_t guest)
{
    void_com_object *o = (void_com_object *)void_com_host(guest);
    if (o->cls == &buffer_class) return (dsbuf_t *)o;
    return ((facet_t *)o)->owner;
}

static uint32_t buffer_query(dsbuf_t *b, uint32_t riid, uint32_t out)
{
    if (void_guid_equal(riid, IID_IDirectSoundBuffer) || void_guid_equal(riid, IID_IDirectSoundBuffer8) ||
        void_guid_equal(riid, IID_IUnknown)) {
        void_com_addref(&b->com); MEM32(out) = b->com.guest; return 0;
    }
    if (void_guid_equal(riid, IID_IDirectSound3DBuffer) && (b->flags & DSBCAPS_CTRL3D) && !b->primary) {
        if (!b->f3d.com.guest) { b->f3d.owner = b; void_com_wrap(&b->f3d.com, &ds3d_class, 0); b->f3d.com.refs = 1000000; }
        void_com_addref(&b->com); MEM32(out) = b->f3d.com.guest; return 0;
    }
    if (void_guid_equal(riid, IID_IDirectSound3DListener) && b->primary) {
        if (!b->f3d.com.guest) { b->f3d.owner = b; void_com_wrap(&b->f3d.com, &listener_class, 0); b->f3d.com.refs = 1000000; }
        void_com_addref(&b->com); MEM32(out) = b->f3d.com.guest; return 0;
    }
    if (void_guid_equal(riid, IID_IDirectSoundNotify) && !b->primary) {
        if (!b->notify.com.guest) { b->notify.owner = b; void_com_wrap(&b->notify.com, &notify_class, 0); b->notify.com.refs = 1000000; }
        void_com_addref(&b->com); MEM32(out) = b->notify.com.guest; return 0;
    }
    char g[40];
    void_guid_format(riid, g);
    void_log("[dsound] QueryInterface(%s) unsupported", g);
    MEM32(out) = 0;
    return E_NOINTERFACE_;
}

#define BUFF buffer_of(THIS_)
COM_METHOD(dsb, QueryInterface, 3) { return buffer_query(BUFF, ARG(1), ARG(2)); }
COM_METHOD(dsb, AddRef, 1) { return void_com_addref(&BUFF->com); }
COM_METHOD(dsb, Release, 1) { return void_com_release(&BUFF->com); }
COM_METHOD(dsb, GetCaps, 2)
{
    dsbuf_t *b = BUFF;
    uint32_t c = ARG(1);
    MEM32(c + 4) = b->flags | 0x8u;   /* LOCSOFTWARE */
    MEM32(c + 8) = b->primary ? 65536u : b->size;
    MEM32(c + 12) = 0; MEM32(c + 16) = 0;
    return DS_OK;
}

static uint32_t buffer_write_cursor(const dsbuf_t *b)
{
    if (!b->playing || !b->size || !b->block) return b->play_pos;
    uint64_t lead = ((uint64_t)b->frequency + 99u) / 100u * b->block;
    if (lead >= b->size) lead = (b->size / 2u / b->block) * b->block;
    return (b->play_pos + (uint32_t)lead) % b->size;
}
COM_METHOD(dsb, GetCurrentPosition, 3)
{
    dsbuf_t *b = BUFF;
    pthread_mutex_lock(&mix_lock);
    uint32_t play = b->play_pos, write = buffer_write_cursor(b);
    pthread_mutex_unlock(&mix_lock);
    if (ARG(1)) MEM32(ARG(1)) = play;
    if (ARG(2)) MEM32(ARG(2)) = b->playing ? write : play;
    return DS_OK;
}
COM_METHOD(dsb, GetFormat, 4)
{
    dsbuf_t *b = BUFF;
    if (ARG(3)) MEM32(ARG(3)) = 18;
    if (!ARG(1)) return DS_OK;
    MEM16(ARG(1)) = 1; MEM16(ARG(1) + 2) = (uint16_t)b->channels; MEM32(ARG(1) + 4) = b->rate;
    MEM32(ARG(1) + 8) = b->rate * b->block; MEM16(ARG(1) + 12) = (uint16_t)b->block; MEM16(ARG(1) + 14) = (uint16_t)b->bits;
    if (ARG(2) >= 18) MEM16(ARG(1) + 16) = 0;
    return DS_OK;
}
COM_METHOD(dsb, GetVolume, 2) { MEM32(ARG(1)) = (uint32_t)BUFF->volume; return DS_OK; }
COM_METHOD(dsb, GetPan, 2) { MEM32(ARG(1)) = (uint32_t)BUFF->pan; return DS_OK; }
COM_METHOD(dsb, GetFrequency, 2) { MEM32(ARG(1)) = BUFF->frequency; return DS_OK; }
COM_METHOD(dsb, GetStatus, 2)
{
    dsbuf_t *b = BUFF;
    uint32_t status = (b->playing ? DSBSTATUS_PLAYING : 0) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0);
    if (dstrace && status != b->last_status) {
        DSTRACE("buffer %08X status %X at %u of %u", b->com.guest, status, b->play_pos, b->size);
        b->last_status = status;
    }
    MEM32(ARG(1)) = status;
    return DS_OK;
}
COM_METHOD(dsb, Initialize, 3) { return DS_OK; }

COM_METHOD(dsb, Lock, 8)
{
    dsbuf_t *b = BUFF;
    uint32_t offset = ARG(1), bytes = ARG(2), flags = ARG(7);
    if (b->primary || !b->size) return DSERR_INVALIDCALL;
    if (flags & 1u) offset = buffer_write_cursor(b); /* DSBLOCK_FROMWRITECURSOR */
    if (flags & 2u) bytes = b->size;                      /* DSBLOCK_ENTIREBUFFER */
    if (offset >= b->size || bytes > b->size) return DSERR_INVALIDPARAM;
    if (dstrace && (b->locks < 4 || !(b->locks & 63)))
        DSTRACE("buffer %08X lock #%u offset %u bytes %u flags %X (play %u)", b->com.guest, b->locks, offset, bytes, flags, b->play_pos);
    ++b->locks;
    uint32_t first = bytes <= b->size - offset ? bytes : b->size - offset;
    MEM32(ARG(3)) = b->data + offset;
    MEM32(ARG(4)) = first;
    if (ARG(5)) MEM32(ARG(5)) = first < bytes ? b->data : 0;
    if (ARG(6)) MEM32(ARG(6)) = bytes - first;
    return DS_OK;
}

COM_METHOD(dsb, Play, 4)
{
    dsbuf_t *b = BUFF;
    pthread_mutex_lock(&mix_lock);
    if (!b->playing) {
        if (b->fresh) memset(b->fresh, 1, b->size / b->block);
        b->last_consumed = 0xFFFFFFFFu;
        b->playing_unlocks = b->stale_run = b->max_stale_run = b->stale_frames = b->source_frames = 0;
        b->last_unlock_ns = void_monotonic_ns();
    }
    b->playing = 1;
    b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
    pthread_mutex_unlock(&mix_lock);
    DSTRACE("buffer %08X play flags %X at %u of %u, %u Hz", b->com.guest, ARG(3), b->play_pos, b->size, b->frequency);
    start_mixer();
    return DS_OK;
}
COM_METHOD(dsb, SetCurrentPosition, 2)
{
    dsbuf_t *b = BUFF;
    pthread_mutex_lock(&mix_lock);
    b->play_pos = b->size ? (ARG(1) % b->size) / (b->block ? b->block : 1) * (b->block ? b->block : 1) : 0;
    b->frac = 0;
    b->last_consumed = 0xFFFFFFFFu;
    pthread_mutex_unlock(&mix_lock);
    DSTRACE("buffer %08X position %u", b->com.guest, ARG(1));
    return DS_OK;
}
COM_METHOD(dsb, SetFormat, 2) { return DS_OK; }
COM_METHOD(dsb, SetVolume, 2) { BUFF->volume = (int32_t)ARG(1); return DS_OK; }
COM_METHOD(dsb, SetPan, 2) { BUFF->pan = (int32_t)ARG(1); return DS_OK; }
COM_METHOD(dsb, SetFrequency, 2)
{
    dsbuf_t *b = BUFF;
    b->frequency = ARG(1) ? ARG(1) : b->rate;
    if (dstrace && b->frequency != b->rate) DSTRACE("buffer %08X frequency %u (rate %u)", b->com.guest, b->frequency, b->rate);
    return DS_OK;
}
COM_METHOD(dsb, Stop, 1)
{
    dsbuf_t *b = BUFF;
    pthread_mutex_lock(&mix_lock);
    int was = b->playing;
    b->playing = 0;
    pthread_mutex_unlock(&mix_lock);
    DSTRACE("buffer %08X stop (was %d) at %u of %u", b->com.guest, was, b->play_pos, b->size);
    if (was) for (uint32_t k = 0; k < b->notify_count; ++k) if (b->notify_offset[k] == 0xFFFFFFFFu) signal_event(b->notify_event[k]);
    return DS_OK;
}
COM_METHOD(dsb, Unlock, 5)
{
    dsbuf_t *b = BUFF;
    uint64_t now = void_monotonic_ns();
    pthread_mutex_lock(&mix_lock);
    if (b->playing && b->last_unlock_ns && now - b->last_unlock_ns > b->max_unlock_gap_ns)
        b->max_unlock_gap_ns = now - b->last_unlock_ns;
    b->last_unlock_ns = now; ++b->unlocks;
    if (b->playing) ++b->playing_unlocks;
    if (b->fresh) for (uint32_t i = 0; i < 2; ++i) {
        uint32_t ptr = ARG(1 + i * 2), bytes = ARG(2 + i * 2);
        if (ptr >= b->data && ptr - b->data <= b->size && bytes <= b->size - (ptr - b->data)) {
            uint32_t offset = ptr - b->data;
            uint32_t first = (offset + b->block - 1) / b->block, end = (offset + bytes) / b->block;
            if (end > first) memset(b->fresh + first, 1, end - first);
        }
    }
    pthread_mutex_unlock(&mix_lock);
    return DS_OK;
}
COM_METHOD(dsb, Restore, 1) { return DS_OK; }
COM_METHOD(dsb, SetFX, 4) { return DSERR_CONTROLUNAVAIL; }
COM_METHOD(dsb, AcquireResources, 4) { return DS_OK; }
COM_METHOD(dsb, GetObjectInPath, 5) { return DSERR_UNSUPPORTED; }

static const void_fn buffer_methods[] = {
    dsb_QueryInterface, dsb_AddRef, dsb_Release, dsb_GetCaps, dsb_GetCurrentPosition, dsb_GetFormat, dsb_GetVolume,
    dsb_GetPan, dsb_GetFrequency, dsb_GetStatus, dsb_Initialize, dsb_Lock, dsb_Play, dsb_SetCurrentPosition,
    dsb_SetFormat, dsb_SetVolume, dsb_SetPan, dsb_SetFrequency, dsb_Stop, dsb_Unlock, dsb_Restore, dsb_SetFX,
    dsb_AcquireResources, dsb_GetObjectInPath,
};
static void_com_class buffer_class = { "IDirectSoundBuffer8", 24, buffer_methods, 0 };

/* -------------------------------------------------------- 3D buffer facet */
static float fval(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
#define B3 (((facet_t *)void_com_host(THIS_))->owner)
COM_METHOD(d3b, QueryInterface, 3) { return buffer_query(B3, ARG(1), ARG(2)); }
COM_METHOD(d3b, AddRef, 1) { return void_com_addref(&B3->com); }
COM_METHOD(d3b, Release, 1) { return void_com_release(&B3->com); }
COM_METHOD(d3b, GetAllParameters, 2) { memset(GPTR(ARG(1) + 4), 0, 60); return DS_OK; }
COM_METHOD(d3b, GetConeAngles, 3) { if (ARG(1)) MEM32(ARG(1)) = 360; if (ARG(2)) MEM32(ARG(2)) = 360; return DS_OK; }
COM_METHOD(d3b, GetConeOrientation, 2) { memset(GPTR(ARG(1)), 0, 12); return DS_OK; }
COM_METHOD(d3b, GetConeOutsideVolume, 2) { MEM32(ARG(1)) = 0; return DS_OK; }
COM_METHOD(d3b, GetMaxDistance, 2) { memcpy(GPTR(ARG(1)), &B3->max_distance, 4); return DS_OK; }
COM_METHOD(d3b, GetMinDistance, 2) { memcpy(GPTR(ARG(1)), &B3->min_distance, 4); return DS_OK; }
COM_METHOD(d3b, GetMode, 2) { MEM32(ARG(1)) = B3->mode3d; return DS_OK; }
COM_METHOD(d3b, GetPosition, 2) { memcpy(GPTR(ARG(1)), B3->pos, 12); return DS_OK; }
COM_METHOD(d3b, GetVelocity, 2) { memset(GPTR(ARG(1)), 0, 12); return DS_OK; }
COM_METHOD(d3b, SetAllParameters, 3)
{
    dsbuf_t *b = B3;
    uint32_t p = ARG(1);
    memcpy(b->pos, GPTR(p + 4), 12);
    memcpy(&b->min_distance, GPTR(p + 52), 4);
    memcpy(&b->max_distance, GPTR(p + 56), 4);
    b->mode3d = MEM32(p + 60);
    return DS_OK;
}
COM_METHOD(d3b, SetConeAngles, 4) { return DS_OK; }
COM_METHOD(d3b, SetConeOrientation, 5) { return DS_OK; }
COM_METHOD(d3b, SetConeOutsideVolume, 3) { return DS_OK; }
COM_METHOD(d3b, SetMaxDistance, 3) { B3->max_distance = fval(ARG(1)); return DS_OK; }
COM_METHOD(d3b, SetMinDistance, 3) { B3->min_distance = fval(ARG(1)); return DS_OK; }
COM_METHOD(d3b, SetMode, 3) { B3->mode3d = ARG(1); return DS_OK; }
COM_METHOD(d3b, SetPosition, 5) { dsbuf_t *b = B3; b->pos[0] = fval(ARG(1)); b->pos[1] = fval(ARG(2)); b->pos[2] = fval(ARG(3)); return DS_OK; }
COM_METHOD(d3b, SetVelocity, 5) { return DS_OK; }
static const void_fn ds3d_methods[] = {
    d3b_QueryInterface, d3b_AddRef, d3b_Release, d3b_GetAllParameters, d3b_GetConeAngles, d3b_GetConeOrientation,
    d3b_GetConeOutsideVolume, d3b_GetMaxDistance, d3b_GetMinDistance, d3b_GetMode, d3b_GetPosition, d3b_GetVelocity,
    d3b_SetAllParameters, d3b_SetConeAngles, d3b_SetConeOrientation, d3b_SetConeOutsideVolume, d3b_SetMaxDistance,
    d3b_SetMinDistance, d3b_SetMode, d3b_SetPosition, d3b_SetVelocity,
};
static void_com_class ds3d_class = { "IDirectSound3DBuffer8", 21, ds3d_methods, 0 };

/* ------------------------------------------------------------- listener */
COM_METHOD(lst, QueryInterface, 3) { return buffer_query(B3, ARG(1), ARG(2)); }
COM_METHOD(lst, AddRef, 1) { return void_com_addref(&B3->com); }
COM_METHOD(lst, Release, 1) { return void_com_release(&B3->com); }
COM_METHOD(lst, GetAllParameters, 2) { memset(GPTR(ARG(1) + 4), 0, 60); return DS_OK; }
COM_METHOD(lst, GetDistanceFactor, 2) { memcpy(GPTR(ARG(1)), &distance_factor, 4); return DS_OK; }
COM_METHOD(lst, GetDopplerFactor, 2) { float one = 1.0f; memcpy(GPTR(ARG(1)), &one, 4); return DS_OK; }
COM_METHOD(lst, GetOrientation, 3) { memcpy(GPTR(ARG(1)), listener_front, 12); memcpy(GPTR(ARG(2)), listener_top, 12); return DS_OK; }
COM_METHOD(lst, GetPosition, 2) { memcpy(GPTR(ARG(1)), listener_pos, 12); return DS_OK; }
COM_METHOD(lst, GetRolloffFactor, 2) { memcpy(GPTR(ARG(1)), &rolloff, 4); return DS_OK; }
COM_METHOD(lst, GetVelocity, 2) { memset(GPTR(ARG(1)), 0, 12); return DS_OK; }
COM_METHOD(lst, SetAllParameters, 3)
{
    uint32_t p = ARG(1);
    memcpy(listener_pos, GPTR(p + 4), 12);
    memcpy(&distance_factor, GPTR(p + 28), 4);
    memcpy(&rolloff, GPTR(p + 32), 4);
    memcpy(listener_front, GPTR(p + 40), 12);
    memcpy(listener_top, GPTR(p + 52), 12);
    return DS_OK;
}
COM_METHOD(lst, SetDistanceFactor, 3) { distance_factor = fval(ARG(1)); return DS_OK; }
COM_METHOD(lst, SetDopplerFactor, 3) { return DS_OK; }
COM_METHOD(lst, SetOrientation, 8)
{
    listener_front[0] = fval(ARG(1)); listener_front[1] = fval(ARG(2)); listener_front[2] = fval(ARG(3));
    listener_top[0] = fval(ARG(4)); listener_top[1] = fval(ARG(5)); listener_top[2] = fval(ARG(6));
    return DS_OK;
}
COM_METHOD(lst, SetPosition, 5) { listener_pos[0] = fval(ARG(1)); listener_pos[1] = fval(ARG(2)); listener_pos[2] = fval(ARG(3)); return DS_OK; }
COM_METHOD(lst, SetRolloffFactor, 3) { rolloff = fval(ARG(1)); return DS_OK; }
COM_METHOD(lst, SetVelocity, 5) { return DS_OK; }
COM_METHOD(lst, CommitDeferredSettings, 1) { return DS_OK; }
static const void_fn listener_methods[] = {
    lst_QueryInterface, lst_AddRef, lst_Release, lst_GetAllParameters, lst_GetDistanceFactor, lst_GetDopplerFactor,
    lst_GetOrientation, lst_GetPosition, lst_GetRolloffFactor, lst_GetVelocity, lst_SetAllParameters,
    lst_SetDistanceFactor, lst_SetDopplerFactor, lst_SetOrientation, lst_SetPosition, lst_SetRolloffFactor,
    lst_SetVelocity, lst_CommitDeferredSettings,
};
static void_com_class listener_class = { "IDirectSound3DListener8", 18, listener_methods, 0 };

/* ---------------------------------------------------------------- notify */
COM_METHOD(ntf, QueryInterface, 3) { return buffer_query(B3, ARG(1), ARG(2)); }
COM_METHOD(ntf, AddRef, 1) { return void_com_addref(&B3->com); }
COM_METHOD(ntf, Release, 1) { return void_com_release(&B3->com); }
COM_METHOD(ntf, SetNotificationPositions, 3)
{
    dsbuf_t *b = B3;
    uint32_t count = ARG(1), list = ARG(2);
    if (count > 64) return DSERR_INVALIDPARAM;
    pthread_mutex_lock(&mix_lock);
    b->notify_count = count;
    for (uint32_t i = 0; i < count; ++i) { b->notify_offset[i] = MEM32(list + 8 * i); b->notify_event[i] = MEM32(list + 8 * i + 4); }
    pthread_mutex_unlock(&mix_lock);
    return DS_OK;
}
static const void_fn notify_methods[] = { ntf_QueryInterface, ntf_AddRef, ntf_Release, ntf_SetNotificationPositions };
static void_com_class notify_class = { "IDirectSoundNotify8", 4, notify_methods, 0 };

/* ------------------------------------------------------------- IDirectSound8 */
typedef struct { void_com_object com; } dsound_t;
#define DSND ((dsound_t *)void_com_host(THIS_))

COM_METHOD(ds, QueryInterface, 3)
{
    if (void_guid_equal(ARG(1), IID_IDirectSound8) || void_guid_equal(ARG(1), IID_IDirectSound) || void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(&DSND->com); MEM32(ARG(2)) = THIS_; return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(ds, AddRef, 1) { return void_com_addref(&DSND->com); }
COM_METHOD(ds, Release, 1) { return void_com_release(&DSND->com); }

static void calibrate_miles_lookahead(void)
{
    uint32_t module = void_module_handle("mss32.dll");
    if (!module || g_cur_func < module || g_cur_func - module >= 0x5E000u) return;
    const char *names[2] = { "_AIL_get_preference@4", "_AIL_set_preference@8" };
    uint32_t functions[2];
    for (int i = 0; i < 2; ++i) {
        uint32_t name = void_gstrdup(names[i]);
        functions[i] = void_module_proc(module, name);
        void_gfree(name);
    }
    if (!functions[0] || !functions[1]) void_fatal("Miles lookahead preference exports unavailable");
    uint32_t args[2] = { 42, 24 };
    /* Deep-buffer devices can return 120 ms of PCM in a burst, exceeding the
     * 12-fragment margin before Miles' next 50-ms service callback. */
    if (void_call_guest(functions[0], 1, args, 1) == 8) {
        uint32_t old = void_call_guest(functions[1], 2, args, 1);
        void_log("[dsound] Miles preference 42 lookahead: %u -> 24 fragments (193.7 ms at 44100 Hz)", old);
    }
}

extern int void_audio_is_paused(void) __attribute__((weak));

/* Guest-thread state: identify the actual started voice, not another finite
 * stream (training narration and Quit can occupy channels 3/4 simultaneously). */
typedef struct { uint32_t manager, handle; int quit; } speech_slot;
static speech_slot speech[2];
static uint32_t preserved_manager, preserved_channel, preserved_handle;

static uint32_t speech_proc(const char *name)
{
    uint32_t module = void_module_handle("mss32.dll");
    if (!module) return 0;
    uint32_t text = void_gstrdup(name), function = void_module_proc(module, text);
    void_gfree(text);
    return function;
}

void stronghold_audio_stream_started(uint32_t manager, uint32_t channel, uint32_t path)
{
    if (channel < 3 || channel > 4 || !void_vm_readable(manager, 0x48)) return;
    speech_slot *slot = &speech[channel - 3];
    slot->manager = manager; slot->handle = MEM32(manager + 4 * (channel + 3)); slot->quit = 0;
    char filename[256]; size_t n = 0;
    while (n < sizeof filename - 1 && void_vm_readable(path + (uint32_t)n, 1)) {
        filename[n] = (char)MEM8(path + (uint32_t)n);
        if (!filename[n++]) break;
    }
    if (!n || filename[n - 1]) return;
    const char *base = filename;
    for (const char *c = filename; *c; ++c) if (*c == '/' || *c == '\\') base = c + 1;
    slot->quit = !strcasecmp(base, "general_quitgame.wav");
    if (preserved_manager == manager && preserved_channel == channel && preserved_handle != slot->handle)
        preserved_handle = 0;
    if (slot->quit) void_log("[audio] quit voice started: channel %u stream %08X file %s", channel, slot->handle, filename);
}

/* An explicit Quit request intentionally replaces tutorial narration. Clearing
 * its protected flag also lets original 47C360 admit the newly requested line. */
void stronghold_audio_quit_request(uint32_t manager)
{
    if (!void_vm_readable(manager, 0x48) || !MEM32(manager + 8)) return;
    uint32_t close = speech_proc("_AIL_close_stream@4");
    if (!close) return;
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
    preserved_handle = 0;
    for (uint32_t channel = 3; channel <= 4; ++channel) {
        uint32_t handle = MEM32(manager + 4 * (channel + 3));
        if (handle && MEM32(manager + 4 * (channel + 8))) void_call_guest(close, 1, &handle, 1);
        MEM32(manager + 4 * (channel + 3)) = MEM32(manager + 4 * (channel + 8)) = 0;
        MEM32(manager + 4 * (channel + 13)) &= 0x7FFFFFFFu;
        speech[channel - 3] = (speech_slot){0};
    }
    g_eax = eax; g_ecx = ecx; g_edx = edx;
}

/* StopAll still stops unrelated voices. Only the just-started Quit can finish
 * over the next menu, with the original Miles timer/driver running normally. */
void stronghold_audio_menu_handoff(uint32_t manager)
{
    preserved_handle = 0;
    if (!void_vm_readable(manager, 0x48)) return;
    for (uint32_t channel = 3; channel <= 4; ++channel) {
        speech_slot *slot = &speech[channel - 3];
        if (slot->manager == manager && slot->quit && slot->handle &&
            slot->handle == MEM32(manager + 4 * (channel + 3)) && MEM32(manager + 4 * (channel + 8))) {
            preserved_manager = manager; preserved_channel = channel; preserved_handle = slot->handle;
            void_log("[audio] menu handoff: preserve quit channel %u stream %08X without waiting", channel, slot->handle);
        }
    }
}

int stronghold_audio_keep_stream(uint32_t manager, uint32_t channel, uint32_t caller)
{
    if (!preserved_handle || manager != preserved_manager || channel != preserved_channel ||
        !void_vm_readable(manager, 0x48) || MEM32(manager + 4 * (channel + 3)) != preserved_handle) return 0;
    /* These are the original explicit replacement paths, not menu StopAll. */
    if (caller == 0x47BDFDu || caller == 0x47BC0Du || caller == 0x47C3B1u ||
        caller == 0x47C1C1u || caller == 0x47C431u || caller == 0x47C241u ||
        caller == 0x47C48Bu || caller == 0x47C29Bu) { preserved_handle = 0; return 0; }
    uint32_t status = speech_proc("_AIL_stream_status@4");
    if (!status) return 0;
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, handle = preserved_handle;
    int keep = void_call_guest(status, 1, &handle, 1) == 4;
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    if (!keep) preserved_handle = 0;
    return keep;
}

/* Wait only for an explicitly selected stream and its already mixed ring tail.
 * Shared by final Quit shutdown and the isolated three-source PCM regression. */
void stronghold_audio_finish_stream(uint32_t manager, uint32_t channel)
{
    if (channel < 3 || channel > 4 || !void_vm_readable(manager, 0x48) ||
        !MEM32(manager + 8) || !mixer_running || (void_audio_is_paused && void_audio_is_paused())) return;
    uint32_t handle = MEM32(manager + 4 * (channel + 3));
    if (!handle) return;
    uint32_t saved_eax = g_eax, saved_ecx = g_ecx, saved_edx = g_edx;
    uint32_t status = speech_proc("_AIL_stream_status@4"), position_fn = speech_proc("_AIL_stream_ms_position@12");
    uint32_t preference_fn = speech_proc("_AIL_get_preference@4");
    uint64_t start = void_monotonic_ns(), deadline = start + 10000000000ull;
    uint32_t scratch = void_galloc(8);
    if (position_fn) {
        uint32_t args[] = { handle, scratch, scratch + 4 };
        MEM32(scratch) = MEM32(scratch + 4) = 0;
        void_call_guest(position_fn, 3, args, 1);
        uint32_t length = MEM32(scratch), position = MEM32(scratch + 4);
        if (length >= position && length <= 120000) deadline = start + ((uint64_t)(length - position) + 2000) * 1000000u;
    }
    while (status && MEM32(manager + 4 * (channel + 3)) == handle &&
           void_call_guest(status, 1, &handle, 1) == 4 && void_monotonic_ns() < deadline &&
           !(void_audio_is_paused && void_audio_is_paused())) {
        void_blocking_begin(); usleep(5000); void_blocking_end();
    }
    void_gfree(scratch);
    uint32_t preference = 12, index = 42;
    if (preference_fn) preference = void_call_guest(preference_fn, 1, &index, 1);
    if (!preference || preference > 95) preference = 95;
    uint64_t frames = ((uint64_t)preference * 356 * MIX_RATE + 44099) / 44100 + 2 * MIX_CHUNK;
    uint64_t target = atomic_load(&mixer_frames) + frames;
    uint64_t ring_deadline = void_monotonic_ns() + frames * 1000000000u / MIX_RATE + 500000000u;
    while (mixer_running && atomic_load(&mixer_frames) < target && void_monotonic_ns() < ring_deadline &&
           !(void_audio_is_paused && void_audio_is_paused())) {
        void_blocking_begin(); usleep(2000); void_blocking_end();
    }
    void_log("[audio] final speech/ring completion: %.1f ms, preference %u, ring frames %llu/%llu",
        (void_monotonic_ns() - start) / 1e6, preference,
        (unsigned long long)atomic_load(&mixer_frames), (unsigned long long)target);
    g_eax = saved_eax; g_ecx = saved_ecx; g_edx = saved_edx;
}

void stronghold_audio_finish_speech(uint32_t manager)
{
    if (!void_vm_readable(manager, 0x48)) return;
    for (uint32_t channel = 3; channel <= 4; ++channel) {
        speech_slot *slot = &speech[channel - 3];
        if (slot->manager == manager && slot->quit && slot->handle &&
            slot->handle == MEM32(manager + 4 * (channel + 3))) stronghold_audio_finish_stream(manager, channel);
    }
    preserved_handle = 0;
}

COM_METHOD(ds, CreateSoundBuffer, 4)
{
    uint32_t desc = ARG(1), out = ARG(2);
    uint32_t flags = MEM32(desc + 4), bytes = MEM32(desc + 8), wfx = MEM32(desc + 16);
    dsbuf_t *b = (dsbuf_t *)calloc(1, sizeof *b);
    b->flags = flags;
    b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    b->min_distance = 1.0f;
    b->max_distance = 1e9f;
    if (wfx) {
        b->channels = MEM16(wfx + 2); b->rate = MEM32(wfx + 4); b->block = MEM16(wfx + 12); b->bits = MEM16(wfx + 14);
    } else {
        b->channels = 2; b->rate = 44100; b->bits = 16; b->block = 4;
    }
    b->frequency = b->rate;
    if (!b->primary) {
        if (!bytes || !b->block) { free(b); return DSERR_INVALIDPARAM; }
        calibrate_miles_lookahead();
        b->size = bytes - bytes % b->block;
        b->data = void_galloc(b->size);
        if (flags & 0x10000u) { /* streaming clients request accurate play cursors */
            b->fresh = (uint8_t *)calloc(b->size / b->block, 1);
            if (!b->fresh) { void_gfree(b->data); free(b); return E_OUTOFMEMORY_; }
        }
    }
    MEM32(out) = void_com_wrap(&b->com, &buffer_class, 0);
    b->com.destroy = buffer_destroy;
    pthread_mutex_lock(&mix_lock);
    b->next = buffers;
    buffers = b;
    pthread_mutex_unlock(&mix_lock);
    if (dstrace || void_verbose)
        void_log("[dsound] buffer %08X flags %08X %u bytes %u Hz %u ch %u bit", MEM32(out), flags, b->size, b->rate, b->channels, b->bits);
    return DS_OK;
}

COM_METHOD(ds, GetCaps, 2)
{
    uint32_t c = ARG(1), size = MEM32(c);
    memset(GPTR(c + 4), 0, size > 4 ? size - 4 : 0);
    MEM32(c + 4) = 0x0F5Fu;     /* continuous rate, mono/stereo, 8/16 bit, emulated=no */
    MEM32(c + 8) = 100; MEM32(c + 12) = 200000;
    MEM32(c + 20) = 64; MEM32(c + 24) = 64; MEM32(c + 28) = 64;     /* max hw mixing buffers */
    return DS_OK;
}

COM_METHOD(ds, DuplicateSoundBuffer, 3)
{
    dsbuf_t *src = buffer_of(ARG(1));
    dsbuf_t *b = (dsbuf_t *)calloc(1, sizeof *b);
    *b = *src;
    memset(&b->com, 0, sizeof b->com);
    memset(&b->f3d, 0, sizeof b->f3d);
    memset(&b->notify, 0, sizeof b->notify);
    b->playing = 0; b->play_pos = 0; b->frac = 0; b->notify_count = 0;
    b->fresh = NULL;
    if (src->fresh) {
        b->fresh = (uint8_t *)malloc(src->size / src->block);
        if (!b->fresh) { free(b); return E_OUTOFMEMORY_; }
        memset(b->fresh, 1, src->size / src->block);
    }
    /* A duplicate shares the PCM in D3D; copy it so lifetimes stay simple. */
    b->data = void_galloc(src->size);
    memcpy(GPTR(b->data), GPTR(src->data), src->size);
    MEM32(ARG(2)) = void_com_wrap(&b->com, &buffer_class, 0);
    b->com.destroy = buffer_destroy;
    pthread_mutex_lock(&mix_lock);
    b->next = buffers;
    buffers = b;
    pthread_mutex_unlock(&mix_lock);
    DSTRACE("buffer %08X duplicates %08X (%u bytes)", MEM32(ARG(2)), ARG(1), b->size);
    return DS_OK;
}

COM_METHOD(ds, SetCooperativeLevel, 3) { return DS_OK; }
COM_METHOD(ds, Compact, 1) { return DS_OK; }
COM_METHOD(ds, GetSpeakerConfig, 2) { MEM32(ARG(1)) = 4; return DS_OK; }   /* DSSPEAKER_STEREO */
COM_METHOD(ds, SetSpeakerConfig, 2) { return DS_OK; }
COM_METHOD(ds, Initialize, 2) { return DS_OK; }
COM_METHOD(ds, VerifyCertification, 2) { MEM32(ARG(1)) = 0; return DS_OK; }

static const void_fn dsound_methods[] = {
    ds_QueryInterface, ds_AddRef, ds_Release, ds_CreateSoundBuffer, ds_GetCaps, ds_DuplicateSoundBuffer,
    ds_SetCooperativeLevel, ds_Compact, ds_GetSpeakerConfig, ds_SetSpeakerConfig, ds_Initialize, ds_VerifyCertification,
};
static void_com_class dsound_class = { "IDirectSound8", 12, dsound_methods, 0 };

/* dsound.dll ordinal 11: DirectSoundCreate8(guid, ppDS8, outer). */
WINAPI_ORDINAL("dsound.dll", DirectSoundCreate8, 11, 3)
{
    dsound_t *d = (dsound_t *)calloc(1, sizeof *d);
    const char *trace = getenv("VOID_DSOUND_TRACE");
    dstrace = trace && *trace == '1';
    MEM32(ARG(1)) = void_com_wrap(&d->com, &dsound_class, 0);
    void_log("[dsound] DirectSoundCreate8 = %08X", MEM32(ARG(1)));
    return DS_OK;
}

WINAPI_ORDINAL("dsound.dll", DirectSoundCreate, 1, 3)
{
    if (!ARG(1)) return E_POINTER_;
    if (ARG(2)) { MEM32(ARG(1)) = 0; return 0x80040110u; } /* CLASS_E_NOAGGREGATION */
    return DirectSoundCreate8_impl();
}

WINAPI_ORDINAL("dsound.dll", DirectSoundEnumerateA, 2, 2)
{
    if (!ARG(0)) return DSERR_INVALIDPARAM;
    uint32_t name = void_gstrdup("Stronghold Android Audio"), module = void_gstrdup("dsound.dll");
    uint32_t args[4] = { 0, name, module, ARG(1) };
    void_call_guest(ARG(0), 4, args, 1);
    void_gfree(name); void_gfree(module);
    return DS_OK;
}

/* No MIDI, recording, CD-audio or system mixer device exists on this port. */
WINAPI_EXPORT(WINMM, midiOutOpen, 5) { if (ARG(0)) MEM32(ARG(0)) = 0; return 6; }
WINAPI_EXPORT(WINMM, midiOutLongMsg, 3) { return 5; }
WINAPI_EXPORT(WINMM, midiOutShortMsg, 2) { return 5; }
WINAPI_EXPORT(WINMM, midiOutClose, 1) { return 5; }
WINAPI_EXPORT(WINMM, midiOutReset, 1) { return 5; }
WINAPI_EXPORT(WINMM, midiOutUnprepareHeader, 3) { return 5; }
WINAPI_EXPORT(WINMM, midiOutPrepareHeader, 3) { return 5; }
WINAPI_EXPORT(WINMM, auxGetDevCapsA, 3) { return 2; }
WINAPI_EXPORT(WINMM, auxGetNumDevs, 0) { return 0; }
WINAPI_EXPORT(WINMM, auxGetVolume, 2) { return 2; }
WINAPI_EXPORT(WINMM, auxSetVolume, 2) { return 2; }
WINAPI_EXPORT(WINMM, mixerClose, 1) { return 5; }
WINAPI_EXPORT(WINMM, mixerGetControlDetailsA, 3) { return 5; }
WINAPI_EXPORT(WINMM, mixerGetLineInfoA, 3) { return 5; }
WINAPI_EXPORT(WINMM, mixerOpen, 5) { if (ARG(0)) MEM32(ARG(0)) = 0; return 6; }
WINAPI_EXPORT(WINMM, mixerGetLineControlsA, 3) { return 5; }
WINAPI_EXPORT(WINMM, mixerGetNumDevs, 0) { return 0; }
WINAPI_EXPORT(WINMM, mixerSetControlDetails, 3) { return 5; }
WINAPI_EXPORT(WINMM, mciSendCommandA, 4) { return 272; } /* MCIERR_DEVICE_NOT_INSTALLED */
WINAPI_EXPORT(WINMM, waveInAddBuffer, 3) { return 5; }
WINAPI_EXPORT(WINMM, waveInPrepareHeader, 3) { return 5; }
WINAPI_EXPORT(WINMM, waveInOpen, 6) { if (ARG(0)) MEM32(ARG(0)) = 0; return 6; }
WINAPI_EXPORT(WINMM, waveInClose, 1) { return 5; }
WINAPI_EXPORT(WINMM, waveInUnprepareHeader, 3) { return 5; }
WINAPI_EXPORT(WINMM, waveInReset, 1) { return 5; }
WINAPI_EXPORT(WINMM, waveInStart, 1) { return 5; }
