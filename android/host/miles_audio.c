/* Native output driver for the pinned Miles mixer. Miles renders each
 * fragment into guest scratch memory and publishes it once to the PCM FIFO. */
#define _GNU_SOURCE
#include "host.h"
#include <arm_neon.h>
#include <errno.h>
#include <stdatomic.h>
#include <strings.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

enum { OUTPUT_RATE = 48000, OUTPUT_CHUNK = 480, FIFO_FRAMES = 32768 };
enum { MILES_DEFAULT_FRAGMENTS = 8, FIFO_DEFAULT_MS = 80 };
enum {
    DIG_PLAYING = 48, DIG_DEFER_STOP = 112, DIG_DEVICE = 104, DIG_DEVICE_ID = 156,
    DIG_EMULATED = 168, DIG_RELEASED = 196, DIG_OPEN = 216,
    DIG_RING_BYTES = 228, DIG_RING_FRAGMENTS = 232, DIG_FRAGMENT_BYTES = 236,
};
enum { MILES_WAVE = 0x21150864, MILES_FRAGMENT_MS = 0x211508B0, MILES_LOOKAHEAD = 0x211508D0 };

static pthread_mutex_t pcm_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint32_t driver, scratch, rate, channels, bits, fragment_frames, target;
    uint32_t head, count, phase;
    unsigned underrun_frames;
    int primed;
    int16_t fifo[FIFO_FRAMES][2];
} pcm;
static pthread_t output_thread;
static _Atomic int output_running;
static _Atomic uint64_t output_frames;
extern void void_restore_host_affinity(void) __attribute__((weak));
extern int void_audio_is_paused(void) __attribute__((weak));
extern uint32_t void_audio_latency_frames(void) __attribute__((weak));
extern int void_audio_compatibility(void) __attribute__((weak));

/* MIXER_copy shifts Miles' signed accumulators by 11 and saturates to PCM16.
 * The 8-bit path uses the high byte, biased by 128. */
int stronghold_miles_copy_bulk(uint32_t source, uint32_t target, uint32_t samples, int wide)
{
    unsigned stride = wide ? 2 : 1;
    uint32_t input = samples * 4, output = samples * stride;
    if (samples < 4 || samples > 16384 ||
        !void_vm_readable(source, input) || !void_vm_writable(target, output) ||
        ((uint64_t)source + input > target && (uint64_t)target + output > source))
        return 0;
    const uint8_t *src = GPTR(source);
    uint8_t *dst = GPTR(target);
    uint32_t i = 0;
    for (; i + 8 <= samples; i += 8) {
        int32x4_t a = vreinterpretq_s32_u8(vld1q_u8(src + i * 4));
        int32x4_t b = vreinterpretq_s32_u8(vld1q_u8(src + (i + 4) * 4));
        int16x8_t pcm16 = vcombine_s16(vqshrn_n_s32(a, 11), vqshrn_n_s32(b, 11));
        if (wide)
            vst1q_u8(dst + i * 2, vreinterpretq_u8_s16(pcm16));
        else
            vst1_u8(dst + i, veor_u8(vreinterpret_u8_s8(vshrn_n_s16(pcm16, 8)), vdup_n_u8(128)));
    }
    for (; i < samples; ++i) {
        int32_t value;
        memcpy(&value, src + i * 4, sizeof value);
        value >>= 11;
        int16_t clipped = value < INT16_MIN ? INT16_MIN : value > INT16_MAX ? INT16_MAX : value;
        if (wide)
            memcpy(dst + i * 2, &clipped, sizeof clipped);
        else
            dst[i] = (uint8_t)((clipped >> 8) + 128);
    }
    return 1;
}

static int16x8_t centered_pcm8(uint8x8_t bytes)
{
    return vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(bytes)), vdupq_n_s16(128));
}

static void mix_four(uint32_t *dst, int32x4_t left, int32x4_t right, int stereo)
{
    uint32x4_t a = vreinterpretq_u32_s32(left), b = vreinterpretq_u32_s32(right);
    if (stereo) {
        vst1q_u32(dst, vaddq_u32(vld1q_u32(dst), vzip1q_u32(a, b)));
        vst1q_u32(dst + 4, vaddq_u32(vld1q_u32(dst + 4), vzip2q_u32(a, b)));
    } else {
        vst1q_u32(dst, vaddq_u32(vld1q_u32(dst), vaddq_u32(a, b)));
    }
}

static void mix_gains(unsigned mode, uint32_t *left, uint32_t *right)
{
    unsigned wide = (mode >> 1) & 1;
    *left = *right = 1u << (wide ? 11 : 19);
    if (mode & 8) {
        /* The lifted tail still applies Miles' 8-bit gain shift to its state. */
        *left = MEM32(0x2114C410) << (wide ? 0 : 8);
        *right = MEM32(0x2114C414) << (wide ? 0 : 8);
    }
    if (!(mode & 5)) *right = 0;
}

/* Equal-rate PCM modes use the original 32-bit wrapping mixer arithmetic. */
int stronghold_miles_merge_bulk(uint32_t source, uint32_t target, uint32_t frames, unsigned mode)
{
    unsigned stereo = mode & 1, wide = (mode >> 1) & 1, paired = (mode >> 2) & 1;
    unsigned step = (1 + wide) * (1 + paired);
    uint32_t in_bytes = frames * step, out_bytes = frames * (1 + stereo) * 4;
    if (frames < 8 || frames > 16384 || (frames & 7) || (target & 3) ||
        !void_vm_readable(source, in_bytes) || !void_vm_writable(target, out_bytes) ||
        ((uint64_t)source + in_bytes > target && (uint64_t)target + out_bytes > source) ||
        (source < 0x2114C420u && (uint64_t)source + in_bytes > 0x2114C410u) ||
        (target < 0x2114C420u && (uint64_t)target + out_bytes > 0x2114C410u))
        return 0;

    uint32_t left, right;
    mix_gains(mode, &left, &right);

    const uint8_t *src = GPTR(source);
    uint32_t *out = GPTR(target);
    for (uint32_t i = 0; i < frames; i += 8) {
        int16x8_t a, b;
        if (paired && wide) {
            int16x8_t lo = vreinterpretq_s16_u8(vld1q_u8(src));
            int16x8_t hi = vreinterpretq_s16_u8(vld1q_u8(src + 16));
            a = vuzp1q_s16(lo, hi);
            b = vuzp2q_s16(lo, hi);
        } else if (paired) {
            uint8x8x2_t pair = vld2_u8(src);
            a = centered_pcm8(pair.val[0]);
            b = centered_pcm8(pair.val[1]);
        } else {
            a = wide ? vreinterpretq_s16_u8(vld1q_u8(src)) : centered_pcm8(vld1_u8(src));
            b = a;
        }
        mix_four(out, vmulq_n_s32(vmovl_s16(vget_low_s16(a)), (int32_t)left),
                 vmulq_n_s32(vmovl_s16(vget_low_s16(b)), (int32_t)right), stereo);
        mix_four(out + 4 * (1 + stereo), vmulq_n_s32(vmovl_high_s16(a), (int32_t)left),
                 vmulq_n_s32(vmovl_high_s16(b), (int32_t)right), stereo);
        src += 8 * step;
        out += 8 * (1 + stereo);
    }
    return 1;
}

/* The pinned nearest-sample loops advance a 16.16 phase and use its carry
 * to choose either source stride. Leave the boundary iteration to Miles. */
uint32_t stronghold_miles_resample_bulk(uint32_t state[3], unsigned mode, uint32_t budget)
{
    uint32_t source = state[0], target = state[1], phase = state[2] << 16;
    uint32_t source_end = MEM32(0x2114C418), target_end = MEM32(0x2114C41C);
    unsigned stereo = mode & 1, wide = (mode >> 1) & 1, paired = (mode >> 2) & 1;
    unsigned input_frame = (1 + wide) * (1 + paired), output_frame = 4 * (1 + stereo);
    if (budget < 8 || source_end <= source || target_end <= target || (target & 3))
        return 0;
    uint32_t source_size = source_end - source, target_size = target_end - target;
    uint32_t frames = target_size / output_frame;
    uint32_t step = MEM32(0x2114C400), before_carry = MEM32(0x2114C408);
    uint32_t after_carry = MEM32(0x2114C404);
    if (frames < 9 || target_size % output_frame || source_size % input_frame ||
        (step & 0xffff) || before_carry % input_frame || after_carry % input_frame ||
        !void_vm_readable(source, source_size) || !void_vm_writable(target, target_size) ||
        (source < target_end && target < source_end) ||
        (source < 0x2114C420u && source_end > 0x2114C400u) ||
        (target < 0x2114C420u && target_end > 0x2114C400u))
        return 0;

    uint32_t left, right;
    mix_gains(mode, &left, &right);
    if (--frames > budget) frames = budget;
    if (frames > 16384) frames = 16384;

    uint32_t done = 0;
    for (; done < frames; ++done) {
        uint32_t next_phase = phase + step;
        uint32_t stride = next_phase < phase ? after_carry : before_carry;
        if (stride >= source_end - source) break;
        uint32_t other = source + paired * (1 + wide);
        int32_t a = wide ? (int16_t)MEM16(source) : (int32_t)MEM8(source) - 128;
        int32_t b = wide ? (int16_t)MEM16(other) : (int32_t)MEM8(other) - 128;
        uint32_t a_scaled = (uint32_t)a * left, b_scaled = (uint32_t)b * right;
        if (stereo) {
            MEM32(target) += a_scaled;
            MEM32(target + 4) += b_scaled;
        } else {
            MEM32(target) += a_scaled + b_scaled;
        }
        source += stride;
        target += output_frame;
        phase = next_phase;
    }
    if (done) {
        state[0] = source;
        state[1] = target;
        state[2] = phase >> 16;
    }
    return done;
}

/* The sink owns pacing. The CLI has no device and advances in real time. */
__attribute__((weak)) void void_audio_output(const int16_t *frames, uint32_t count)
{
    (void)frames;
    uint64_t duration = (uint64_t)count * 1000000000u / OUTPUT_RATE;
    struct timespec delay = { (time_t)(duration / 1000000000u), (long)(duration % 1000000000u) };
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}

__attribute__((weak)) void void_audio_wait_ready(void) {}
__attribute__((weak)) void void_audio_drain(void) {}
__attribute__((weak)) void void_audio_begin_shutdown(void) {}

static void reset_fifo(void)
{
    pcm.head = pcm.count = pcm.phase = 0;
    pcm.primed = 0;
}

static void publish_fragment(void)
{
    const uint8_t *bytes = GPTR(pcm.scratch);
    uint32_t tail = (pcm.head + pcm.count) % FIFO_FRAMES;
    uint32_t stride = pcm.bits / 8;
    for (uint32_t i = 0; i < pcm.fragment_frames; ++i) {
        for (uint32_t c = 0; c < 2; ++c) {
            const uint8_t *sample = bytes + (c < pcm.channels ? c * stride : 0);
            int16_t value = 0;
            if (pcm.bits == 16) memcpy(&value, sample, sizeof value);
            else value = (int16_t)(((int)*sample - 128) * 256);
            pcm.fifo[tail][c] = value;
        }
        bytes += pcm.channels * stride;
        tail = (tail + 1) % FIFO_FRAMES;
    }
    pcm.count += pcm.fragment_frames;
    pcm.primed = 1;
}

static void read_pcm(int16_t *out, uint32_t frames)
{
    uint32_t i = 0;
    for (; i < frames && pcm.count; ++i) {
        uint32_t next = pcm.count > 1 ? (pcm.head + 1) % FIFO_FRAMES : pcm.head;
        for (unsigned c = 0; c < 2; ++c) {
            int32_t a = pcm.fifo[pcm.head][c], b = pcm.fifo[next][c];
            out[2 * i + c] = (int16_t)((a * (int32_t)(OUTPUT_RATE - pcm.phase) +
                                        b * (int32_t)pcm.phase) / OUTPUT_RATE);
        }
        pcm.phase += pcm.rate;
        uint32_t step = pcm.phase / OUTPUT_RATE;
        pcm.phase %= OUTPUT_RATE;
        if (step >= pcm.count) {
            pcm.count = 0;
            pcm.phase = 0;
        } else {
            pcm.head = (pcm.head + step) % FIFO_FRAMES;
            pcm.count -= step;
        }
    }
    if (pcm.primed) pcm.underrun_frames += frames - i;
    memset(out + 2 * i, 0, (frames - i) * 2 * sizeof *out);
}

static uint32_t queued_pcm_frames(void)
{
    pthread_mutex_lock(&pcm_lock);
    uint32_t frames = pcm.rate ? (uint32_t)(((uint64_t)pcm.count * OUTPUT_RATE + pcm.rate - 1) / pcm.rate) : 0;
    pthread_mutex_unlock(&pcm_lock);
    return frames;
}

static void *output_main(void *unused)
{
    (void)unused;
    if (void_restore_host_affinity) void_restore_host_affinity();
#ifdef __ANDROID__
    /* Keep the short PCM writer from competing at the game's CPU priority. */
    if (setpriority(PRIO_PROCESS, 0, -16)) void_log("[miles] audio thread priority: %s", strerror(errno));
#endif
    int16_t out[2 * OUTPUT_CHUNK];
    unsigned ticks = 0;
    while (output_running) {
        void_audio_wait_ready();
        if (!output_running) break;
        uint32_t count = 0, target = 0, rate = 0;
        unsigned underruns = 0;
        pthread_mutex_lock(&pcm_lock);
        read_pcm(out, OUTPUT_CHUNK);
        if (++ticks % 1000 == 0 && pcm.driver) {
            count = pcm.count;
            target = pcm.target;
            rate = pcm.rate;
            underruns = pcm.underrun_frames;
            pcm.underrun_frames = 0;
        }
        pthread_mutex_unlock(&pcm_lock);
        if (rate) void_log("[miles] PCM FIFO: queued %u/%u at %u Hz, %u silent output frames from underruns",
                           count, target, rate, underruns);
        void_audio_output(out, OUTPUT_CHUNK);
        atomic_fetch_add(&output_frames, OUTPUT_CHUNK);
    }
    return NULL;
}

void void_audio_shutdown(void)
{
    void_audio_begin_shutdown();
    if (atomic_exchange(&output_running, 0)) pthread_join(output_thread, NULL);
    void_audio_drain();
}

/* Fastcall: ECX is DIG_DRIVER, EDX points at its legacy device field and
 * five arguments are popped. Miles allocates everything above the output. */
void sub_21111A50(void)
{
    uint32_t driver = g_ecx, device = g_edx, format = ARG(1);
    g_esp += 24;
    g_eax = 1;
    if (!void_vm_writable(driver, 0x2D4) || !void_vm_writable(device, 4) ||
        !void_vm_readable(format, 16) || MEM16(format) != 1 || MEM32(driver + DIG_OPEN)) return;
    uint32_t rate = MEM32(format + 4), channels = MEM16(format + 2), bits = MEM16(format + 14);
    if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2 || (bits != 8 && bits != 16)) return;
    uint32_t block = channels * (bits / 8);
    if (MEM16(format + 12) != block || MEM32(format + 8) != rate * block) return;
    uint32_t ms = MEM32(MILES_FRAGMENT_MS);
    if (ms < 1 || ms > 40) return;
    uint32_t bytes = ((ms * rate * block / 1000u) + 15u) & ~15u;
    uint32_t fragment = bytes / block;
    uint32_t fragments = MEM32(MILES_LOOKAHEAD);
    uint32_t max_fragments = FIFO_FRAMES / fragment - 1;
    if (fragments < 2 || fragments > max_fragments) return;
    uint32_t fifo_fragments = fragments;
    if (fragments == MILES_DEFAULT_FRAGMENTS) {
        /* Cover the 50 ms Miles timer plus scheduler jitter without mixing
         * hundreds of milliseconds before a new game action can be heard. */
        /* The compatibility mixer can pull a 40 ms batch around the 50 ms timer. */
        uint32_t reserve_ms = void_audio_compatibility && void_audio_compatibility() ? 100 : FIFO_DEFAULT_MS;
        uint32_t frames = (rate * reserve_ms + 999u) / 1000u;
        uint32_t reserve = (frames + fragment - 1) / fragment;
        if (reserve > max_fragments) reserve = max_fragments;
        if (reserve > fifo_fragments) fifo_fragments = reserve;
    }
    uint32_t scratch = void_galloc(bytes);
    if (!scratch) { g_eax = 7; return; }

    pthread_mutex_lock(&pcm_lock);
    /* shortcut: the three games and Bink share one DIG_DRIVER per process.
     * Support multiple producers if a future executable opens more than one. */
    if (pcm.driver) {
        pthread_mutex_unlock(&pcm_lock);
        void_gfree(scratch);
        return;
    }
    pcm.driver = driver;
    pcm.scratch = scratch;
    pcm.rate = rate;
    pcm.channels = channels;
    pcm.bits = bits;
    pcm.fragment_frames = fragment;
    pcm.target = fifo_fragments * fragment;
    pcm.underrun_frames = 0;
    reset_fifo();
    pthread_mutex_unlock(&pcm_lock);

    MEM32(MILES_WAVE) = 0;
    MEM32(MILES_LOOKAHEAD) = fragments;
    MEM32(device) = 0;
    MEM32(driver + DIG_DEVICE_ID) = 0;
    MEM32(driver + DIG_EMULATED) = 0;
    /* Miles' minimum sample buffer sizing still reads the fragment geometry. */
    MEM32(driver + DIG_FRAGMENT_BYTES) = bytes;
    MEM32(driver + DIG_RING_FRAGMENTS) = fragments + 1;
    MEM32(driver + DIG_RING_BYTES) = (fragments + 1) * bytes;
    MEM32(driver + DIG_OPEN) = 1;
    g_eax = 0;
    void_log("[miles] native PCM: %u Hz %u ch %u-bit, fragment %u frames, FIFO target %u ms",
             rate, channels, bits, fragment, (unsigned)((uint64_t)fifo_fragments * fragment * 1000u / rate));
}

/* Thiscall: sample start/resume enables the existing Miles service. */
void sub_21112330(void)
{
    uint32_t driver = g_ecx;
    MEM32(driver + DIG_DEFER_STOP) = 0;
    g_eax = MEM32(driver + DIG_PLAYING);
    if (MEM32(driver + DIG_OPEN)) MEM32(driver + DIG_PLAYING) = 1;
    g_esp += 4;
}

/* Called inside Miles' original mix guard. The output thread never calls guest
 * code. Release the FIFO mutex before Miles can yield or call an EOS callback. */
void stronghold_audio_miles_fill(uint32_t driver)
{
    uint32_t ecx = g_ecx, edx = g_edx;
    pthread_mutex_lock(&pcm_lock);
    uint32_t budget = pcm.driver && pcm.driver == driver ? pcm.target / pcm.fragment_frames : 0;
    pthread_mutex_unlock(&pcm_lock);
    for (uint32_t i = 0; i < budget; ++i) {
        pthread_mutex_lock(&pcm_lock);
        uint32_t scratch = pcm.driver == driver && pcm.count < pcm.target ? pcm.scratch : 0;
        pthread_mutex_unlock(&pcm_lock);
        if (!scratch || !MEM32(driver + DIG_PLAYING)) break;
        g_ecx = driver;
        g_edx = scratch;
        void_call_guest(0x21112C90, 0, NULL, 1);
        pthread_mutex_lock(&pcm_lock);
        if (pcm.driver == driver && pcm.scratch == scratch && MEM32(driver + DIG_PLAYING))
            publish_fragment();
        pthread_mutex_unlock(&pcm_lock);
    }
    if (!output_running && budget) {
        pthread_mutex_lock(&pcm_lock);
        int start = pcm.driver == driver && pcm.count != 0;
        pthread_mutex_unlock(&pcm_lock);
        if (start) {
            output_running = 1;
            if (pthread_create(&output_thread, NULL, output_main, NULL))
                void_fatal("native audio thread creation failed");
        }
    }
    g_ecx = ecx;
    g_edx = edx;
}

/* Fastcall reset and close, both reached by Miles' release/reacquire flow. */
void sub_21111EB0(void)
{
    pthread_mutex_lock(&pcm_lock);
    if (pcm.driver == g_ecx) reset_fifo();
    pthread_mutex_unlock(&pcm_lock);
    g_eax = 0;
    g_esp += 4;
}

void sub_21111E70(void)
{
    uint32_t driver = g_ecx, scratch = 0;
    pthread_mutex_lock(&pcm_lock);
    if (pcm.driver == driver) {
        scratch = pcm.scratch;
        pcm.driver = pcm.scratch = 0;
        reset_fifo();
    }
    pthread_mutex_unlock(&pcm_lock);
    if (scratch) {
        MEM32(driver + DIG_PLAYING) = MEM32(driver + DIG_OPEN) = MEM32(driver + DIG_DEVICE) = 0;
        MEM32(driver + DIG_RELEASED) = 1;
        void_gfree(scratch);
    }
    g_eax = scratch ? 0 : 1;
    g_esp += 4;
}

/* Miles returns the driver's name through a Win32-shaped legacy record. */
void sub_211119D0(void)
{
    uint32_t caps = ARG(0);
    static const char name[] = "Android PCM - MSS Mixer";
    g_eax = 11;
    if (ARG(1) >= 52 && void_vm_writable(caps, 52)) {
        memset(GPTR(caps), 0, 52);
        memcpy(GPTR(caps + 8), name, sizeof name);
        g_eax = 0;
    }
    g_esp += 12;
}

/* Bink asks Miles for the latency to use for A/V synchronisation. */
void sub_21114EC0(void)
{
    uint32_t driver = ARG(0);
    uint32_t queued = driver ? queued_pcm_frames() : 0;
    if (driver && void_audio_latency_frames) queued += void_audio_latency_frames();
    g_eax = (queued + OUTPUT_RATE / 1000u - 1) / (OUTPUT_RATE / 1000u);
    g_esp += 8;
}

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

/* Wait only for an explicitly selected stream and its already mixed PCM tail.
 * Shared by final Quit shutdown and the isolated three-source PCM regression. */
void stronghold_audio_finish_stream(uint32_t manager, uint32_t channel)
{
    if (channel < 3 || channel > 4 || !void_vm_readable(manager, 0x48) ||
        !MEM32(manager + 8) || !pcm.driver || (void_audio_is_paused && void_audio_is_paused())) return;
    uint32_t handle = MEM32(manager + 4 * (channel + 3));
    if (!handle) return;
    uint32_t saved_eax = g_eax, saved_ecx = g_ecx, saved_edx = g_edx;
    uint32_t status = speech_proc("_AIL_stream_status@4"), position_fn = speech_proc("_AIL_stream_ms_position@12");
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
    uint32_t frames = queued_pcm_frames() + OUTPUT_CHUNK;
    uint64_t target = atomic_load(&output_frames) + frames;
    uint64_t deadline_ns = void_monotonic_ns() + (uint64_t)frames * 1000000000u / OUTPUT_RATE + 500000000u;
    while (output_running && atomic_load(&output_frames) < target && void_monotonic_ns() < deadline_ns &&
           !(void_audio_is_paused && void_audio_is_paused())) {
        void_blocking_begin(); usleep(2000); void_blocking_end();
    }
    void_log("[audio] final speech/FIFO completion: %.1f ms, queued tail %u frames",
        (void_monotonic_ns() - start) / 1e6, frames);
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

