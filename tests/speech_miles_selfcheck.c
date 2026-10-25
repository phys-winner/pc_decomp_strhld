/* Real lifted Miles -> Win32 HLE mixer -> memory capture; no Android sink/UI.
 * Usage: speech-miles-selfcheck PREF42 BURST_MS OUTPUT_DIR [MODE]
 * MODE: 0 normal, 1 close stream at EOS, 2 finish helper then driver close
 * after 20 ms, 3 immediate driver close (negative control),
 * 4 tutorial->Quit/menu handoff, 5 unrelated narration at shutdown,
 * 6 explicit replacement may cancel a preserved Quit.
 * Link with the normal shell objects except main.o/android_main.o. */
#define _GNU_SOURCE
#include "host.h"
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000u
void stronghold_audio_finish_speech(uint32_t);
void stronghold_audio_finish_stream(uint32_t,uint32_t);
void stronghold_audio_stream_started(uint32_t,uint32_t,uint32_t);
void stronghold_audio_quit_request(uint32_t);
void stronghold_audio_menu_handoff(uint32_t);
int stronghold_audio_keep_stream(uint32_t,uint32_t,uint32_t);
#define CAPACITY (RATE * 8u)
static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t capture[CAPACITY * 2u];
static uint32_t captured, burst_ms = 10;
static int capturing, overflow;
static uint64_t output_ticks;

static void sleep_ms(unsigned ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t)) {}
}

void void_audio_output(const int16_t *frames, uint32_t count)
{
    pthread_mutex_lock(&capture_lock);
    if (capturing) {
        if (count <= CAPACITY - captured) {
            memcpy(capture + captured * 2u, frames, count * 2u * sizeof *frames);
            captured += count;
        } else overflow = 1;
    }
    ++output_ticks;
    uint64_t ticks = output_ticks;
    pthread_mutex_unlock(&capture_lock);
    /* A burst sink returns five 10 ms blocks together, then waits 50 ms. */
    if (burst_ms == 10 || ticks % 5 == 0) sleep_ms(burst_ms);
}

void void_process_exit(uint32_t code, int run_detach)
{
    (void)run_detach;
    void_log("[speech] unexpected process exit %u", code);
    _exit(code ? (int)code : 99);
}

static uint32_t proc(uint32_t module, const char *name)
{
    uint32_t text = void_gstrdup(name);
    uint32_t address = void_module_proc(module, text);
    void_gfree(text);
    if (!address) void_fatal("speech export absent: %s", name);
    return address;
}

static void wait_unlocked(unsigned ms)
{
    void_machine_leave();
    sleep_ms(ms);
    void_machine_enter();
}

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5) return 64;
    int mode = argc == 5 ? atoi(argv[4]) : 0;
    if (mode < 0 || mode > 6) return 64;
    int close_at_eos = mode == 1 || mode == 4, final_cleanup = mode == 2 || mode == 3 || mode == 6;
    uint32_t pref = (uint32_t)strtoul(argv[1], NULL, 10);
    burst_ms = (uint32_t)strtoul(argv[2], NULL, 10);
    if ((pref != 12 && pref != 24) || (burst_ms != 10 && burst_ms != 50)) return 64;
    if (mkdir(argv[3], 0775) && errno != EEXIST) return 65;
    setvbuf(stderr, NULL, _IONBF, 0);
    void_install_fault_handlers();
    if (!void_guest_reserve()) return 66;
    void_threads_init();
    void_dispatch_init();
    if (!void_files_init("/sdcard/Stronghold/game", argv[3])) return 67;
    void void_files_std_init(void);
    void_files_std_init();
    if (!void_modules_init("/sdcard/Stronghold/lifted")) return 68;
    void_machine_enter();
    uint32_t module = void_module_load("Mss32.dll");
    if (module != 0x21100000u) return 69;
    uint32_t startup = proc(module, "_AIL_startup@0");
    uint32_t open = proc(module, "_AIL_waveOutOpen@16");
    uint32_t close_driver = proc(module, "_AIL_close_digital_driver@4");
    uint32_t set_pref = proc(module, "_AIL_set_preference@8");
    uint32_t open_stream = proc(module, "_AIL_open_stream@12");
    uint32_t loop = proc(module, "_AIL_set_stream_loop_count@8");
    uint32_t volume = proc(module, "_AIL_set_stream_volume@8");
    uint32_t start = proc(module, "_AIL_start_stream@4");
    uint32_t status = proc(module, "_AIL_stream_status@4");
    uint32_t close_stream = proc(module, "_AIL_close_stream@4");
    uint32_t shutdown = proc(module, "_AIL_shutdown@0");
    if (!void_call_guest(startup, 0, NULL, 1)) void_fatal("Miles startup failed");
    uint32_t format = void_galloc(20), driver_out = void_galloc(4);
    memset(GPTR(format), 0, 20);
    MEM16(format) = 1; MEM16(format + 2) = 2;
    MEM32(format + 4) = 44100; MEM32(format + 8) = 176400;
    MEM16(format + 12) = 4; MEM16(format + 14) = 16;
    uint32_t args[4] = { driver_out, 0, 0xFFFFFFFFu, format };
    if (void_call_guest(open, 4, args, 1) || !MEM32(driver_out)) void_fatal("Miles driver failed");
    uint32_t driver = MEM32(driver_out);
    args[0] = 42; args[1] = pref;
    uint32_t previous = void_call_guest(set_pref, 2, args, 1);
    void_log("[speech] pref42 %u -> %u, fake sink %u ms, driver %08X, mode %d", previous, pref, burst_ms, driver, mode);
    uint32_t policy_manager = 0;
    if (mode >= 4) {
        policy_manager = void_galloc(0x48); memset(GPTR(policy_manager),0,0x48);
        MEM32(policy_manager+8)=1;
        uint32_t training_path = void_gstrdup("fx\\speech\\game_running.wav");
        args[0]=driver;args[1]=training_path;args[2]=0;
        uint32_t training=void_call_guest(open_stream,3,args,1);
        if (!training) void_fatal("training fixture stream failed");
        args[0]=training;args[1]=0;void_call_guest(loop,2,args,1);
        MEM32(policy_manager+0x18)=training;MEM32(policy_manager+0x2c)=1;MEM32(policy_manager+0x40)=0x80000001u;
        void_call_guest(start,1,args,1);
        stronghold_audio_stream_started(policy_manager,3,training_path);
        uint64_t before=void_monotonic_ns();
        if (mode == 5) {
            stronghold_audio_finish_speech(policy_manager);
            double delay=(void_monotonic_ns()-before)/1e6;
            if (delay>50) void_fatal("unrelated narration blocked shutdown %.1f ms",delay);
            void_log("[speech-policy] unrelated narration finish %.3f ms PASS",delay);
            args[0]=training;void_call_guest(close_stream,1,args,1);
            args[0]=driver;void_call_guest(close_driver,1,args,1);
            void_call_guest(shutdown,0,NULL,1);void_machine_leave();
            void void_dsound_shutdown(void);void_dsound_shutdown();
            return 0;
        }
        stronghold_audio_quit_request(policy_manager);
        double delay=(void_monotonic_ns()-before)/1e6;
        if (delay>50 || MEM32(policy_manager+0x18) || MEM32(policy_manager+0x2c) || (MEM32(policy_manager+0x40)&0x80000000u))
            void_fatal("Quit failed to replace protected training %.1f ms",delay);
        void_log("[speech-policy] explicit Quit replaced training %.3f ms PASS",delay);
        void_gfree(training_path);
        /* Fixture isolation: let already mixed training PCM leave the ring.
         * This is not a production wait or the menu transition under test. */
        wait_unlocked(150);
    }
    const char *names[] = { "game_paused.wav", "game_running.wav", "general_quitgame.wav" };
    for (unsigned i = 0; i < 3; ++i) {
        if (mode >= 4 && i < 2) continue;
        if (i && final_cleanup && mode < 4) {
            args[0] = driver_out; args[1] = 0; args[2] = 0xFFFFFFFFu; args[3] = format;
            if (void_call_guest(open, 4, args, 1) || !MEM32(driver_out)) void_fatal("Miles reopen failed");
            driver = MEM32(driver_out);
            args[0] = 42; args[1] = pref; void_call_guest(set_pref, 2, args, 1);
        }
        char filename[256];
        snprintf(filename, sizeof filename, "fx\\speech\\%s", names[i]);
        uint32_t path = void_gstrdup(filename);
        args[0] = driver; args[1] = path; args[2] = 0;
        uint32_t stream = void_call_guest(open_stream, 3, args, 1);
        if (!stream) void_fatal("Miles stream failed: %s", filename);
        args[0] = stream; args[1] = 1;
        void_call_guest(loop, 2, args, 1);
        args[1] = 100; void_call_guest(volume, 2, args, 1);
        pthread_mutex_lock(&capture_lock);
        captured = 0; capturing = 1; overflow = 0;
        pthread_mutex_unlock(&capture_lock);
        void_call_guest(start, 1, args, 1);
        if (mode == 4 || mode == 6) {
            MEM32(policy_manager+0x1c)=stream;MEM32(policy_manager+0x30)=MEM32(policy_manager+0x44)=1;
            stronghold_audio_stream_started(policy_manager,4,path);
            uint64_t before=void_monotonic_ns();stronghold_audio_menu_handoff(policy_manager);
            double delay=(void_monotonic_ns()-before)/1e6;
            if (delay>50 || !stronghold_audio_keep_stream(policy_manager,4,0x47b77fu) || stronghold_audio_keep_stream(policy_manager,3,0x47b773u))
                void_fatal("menu handoff invalid or blocking %.1f ms",delay);
            void_log("[speech-policy] menu handoff %.3f ms, only Quit preserved PASS",delay);
            if (mode == 6) {
                if (stronghold_audio_keep_stream(policy_manager,4,0x47bdfdu) || stronghold_audio_keep_stream(policy_manager,4,0x47b77fu))
                    void_fatal("explicit replacement could not cancel preserved Quit");
                void_log("[speech-policy] intentional replacement allowed PASS");
            }
        }
        uint64_t began = void_monotonic_ns();
        uint32_t state, eos_frame;
        if (final_cleanup) {
            wait_unlocked(20);
            if (mode == 2) {
                uint32_t manager = void_galloc(0x48);
                memset(GPTR(manager), 0, 0x48);
                MEM32(manager + 8) = MEM32(manager + 0x2C) = MEM32(manager + 0x40) = 1;
                MEM32(manager + 0x18) = stream;
                g_eax = 0x12345678; g_ecx = 0x23456789; g_edx = 0x3456789A;
                stronghold_audio_finish_stream(manager,3);
                if (g_eax != 0x12345678 || g_ecx != 0x23456789 || g_edx != 0x3456789A)
                    void_fatal("speech completion helper changed registers");
                void_gfree(manager);
            }
            state = void_call_guest(status, 1, args, 1);
            pthread_mutex_lock(&capture_lock); eos_frame = captured; pthread_mutex_unlock(&capture_lock);
            void_call_guest(close_stream, 1, args, 1);
            args[0] = driver; void_call_guest(close_driver, 1, args, 1);
        } else {
            do {
                wait_unlocked(5);
                state = void_call_guest(status, 1, args, 1);
                if (void_monotonic_ns() - began > 5000000000ull) void_fatal("Miles EOS timeout %s", names[i]);
            } while (state == 4);
            pthread_mutex_lock(&capture_lock); eos_frame = captured; pthread_mutex_unlock(&capture_lock);
            if (mode == 4 && stronghold_audio_keep_stream(policy_manager,4,0x47b77fu)) void_fatal("EOS Quit remained protected");
            if (close_at_eos) void_call_guest(close_stream, 1, args, 1);
            /* Keep driver alive to distinguish stream close from teardown. */
            wait_unlocked(close_at_eos ? 300 : 1000);
        }
        pthread_mutex_lock(&capture_lock);
        capturing = 0;
        uint32_t frames = captured;
        int lost = overflow;
        pthread_mutex_unlock(&capture_lock);
        char output[1024];
        snprintf(output, sizeof output, "%s/%s.pcm", argv[3], names[i]);
        FILE *f = fopen(output, "wb");
        if (!f || fwrite(capture, 4, frames, f) != frames || fclose(f)) void_fatal("PCM write failed");
        void_log("[speech] %s state %u eos_frame %u captured %u overflow %d output %s", names[i], state, eos_frame, frames, lost, output);
        if (lost) return 70;
        if (!close_at_eos && !final_cleanup) void_call_guest(close_stream, 1, args, 1);
        void_gfree(path);
        wait_unlocked(100);
    }
    if (!final_cleanup) { args[0] = driver; void_call_guest(close_driver, 1, args, 1); }
    void_call_guest(shutdown, 0, NULL, 1);
    void_machine_leave();
    void void_dsound_shutdown(void);
    void_dsound_shutdown();
    void_log("[speech] PASS capture completed, no AAudio backend");
    return 0;
}
