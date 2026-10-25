/* Standalone ARM64 check: compile with function/data sections and --gc-sections. */
#include <assert.h>
#include <sys/mman.h>
#include "../android/host/dsound.c"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;
int void_verbose;
static dsbuf_t *test_ds;
static uint32_t test_preference = 8, preference_writes;
uint64_t void_monotonic_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
void *void_com_host(uint32_t guest) { (void)guest; assert(test_ds); return test_ds; }
uint32_t void_com_addref(void_com_object *o) { (void)o; assert(0); return 0; }
uint32_t void_com_release(void_com_object *o) { (void)o; assert(0); return 0; }
int void_guid_equal(uint32_t va, const uint8_t guid[16]) { (void)va; (void)guid; assert(0); return 0; }
void void_guid_format(uint32_t va, char out[40]) { (void)va; (void)out; assert(0); }
uint32_t void_com_wrap(void_com_object *o, void_com_class *cls, uint32_t extra) { (void)o; (void)cls; (void)extra; assert(0); return 0; }

int void_vm_readable(uint32_t p, uint32_t n) { return p >= 0x1000 && n <= 0x10000u - p; }
int void_vm_writable(uint32_t p, uint32_t n) { return void_vm_readable(p, n); }
void_object *void_handle_get(uint32_t h, int kind) { (void)h; (void)kind; assert(0); return NULL; }
void void_object_signal(void_object *o) { (void)o; assert(0); }
void void_post_message(uint32_t h, uint32_t m, uint32_t w, uint32_t l) { (void)h; (void)m; (void)w; (void)l; assert(0); }
void void_post_thread_message(uint32_t t, uint32_t m, uint32_t w, uint32_t l) { (void)t; (void)m; (void)w; (void)l; assert(0); }
void void_machine_enter(void) { assert(0); }
void void_machine_leave(void) { assert(0); }
uint32_t void_call_guest(uint32_t va, int n, const uint32_t *a, int pop)
{
    assert(pop == 1 && a[0] == 42);
    if (va == 0x1111) { assert(n == 1); return test_preference; }
    assert(va == 0x2222 && n == 2);
    uint32_t old = test_preference; test_preference = a[1]; ++preference_writes; return old;
}
uint32_t void_module_handle(const char *name) { assert(!strcmp(name, "mss32.dll")); return 0x21100000; }
uint32_t void_gstrdup(const char *name) { strcpy((char *)GPTR(0x7000), name); return 0x7000; }
void void_gfree(uint32_t p) { assert(p == 0x7000); }
uint32_t void_module_proc(uint32_t module, uint32_t name)
{
    assert(module == 0x21100000);
    if (!strcmp((char *)GPTR(name), "_AIL_get_preference@4")) return 0x1111;
    assert(!strcmp((char *)GPTR(name), "_AIL_set_preference@8")); return 0x2222;
}
void void_log(const char *fmt, ...) { (void)fmt; }
_Noreturn void void_fatal(const char *fmt, ...) { fputs(fmt, stderr); abort(); }

static void args(uint32_t handle, uint32_t header, uint32_t length)
{
    g_esp = 0x1000;
    ARG(0) = handle; ARG(1) = header; ARG(2) = length;
}

int main(void)
{
    assert(mmap((void *)(uintptr_t)VOID_GUEST_BASE, 0x10000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)VOID_GUEST_BASE);
    wave_device *w = &wave_devices[0];
    *w = (wave_device){ .active = 1, .handle = 0x71000000, .channels = 1, .bits = 16,
        .rate = 48000, .block = 2, .volume = 0xFFFFFFFFu };
    uint32_t hdr = 0x2000, pcm = 0x3000;
    MEM32(hdr) = pcm; MEM32(hdr + 4) = 8;
    MEM16(pcm) = 32767; MEM16(pcm + 2) = 0x8000; MEM16(pcm + 4) = 16384; MEM16(pcm + 6) = 0;
    args(w->handle, hdr, 32);
    assert(waveOutWrite_impl() == 34); /* unprepared */
    assert(waveOutPrepareHeader_impl() == 0);
    assert(waveOutWrite_impl() == 0);
    assert(waveOutWrite_impl() == 33); /* already queued */
    assert(waveOutUnprepareHeader_impl() == 33);
    assert(waveOutClose_impl() == 33);
    float mix[16] = {0};
    assert(waveOutPause_impl() == 0);
    wave_mix(mix, 2);
    assert(w->frame == 0 && mix[0] == 0);
    assert(waveOutRestart_impl() == 0);
    wave_mix(mix, 4);
    assert(fabsf(mix[0] - 32767.0f / 32768) < 1e-6f && mix[0] == mix[1]);
    assert(mix[2] == -1 && mix[4] == 0.5f && mix[6] == 0);
    assert(!w->head && (MEM32(hdr + 16) & 0x13u) == 3);
    assert(waveOutUnprepareHeader_impl() == 0 && !(MEM32(hdr + 16) & 2u));

    /* Two adjacent 8-bit packets at half the output rate retain sample order. */
    w->bits = 8; w->block = 1; w->rate = 24000;
    MEM32(hdr + 4) = 2; MEM8(pcm) = 128; MEM8(pcm + 1) = 255;
    assert(waveOutPrepareHeader_impl() == 0 && waveOutWrite_impl() == 0);
    uint32_t hdr2 = 0x2040;
    MEM32(hdr2) = pcm + 2; MEM32(hdr2 + 4) = 1; MEM8(pcm + 2) = 0;
    args(w->handle, hdr2, 32);
    assert(waveOutPrepareHeader_impl() == 0 && waveOutWrite_impl() == 0);
    memset(mix, 0, sizeof mix);
    wave_mix(mix, 6);
    assert(mix[0] == 0 && mix[2] == 0 && mix[4] == 127.0f / 128 && mix[6] == mix[4]);
    assert(mix[8] == -1 && mix[10] == -1 && !w->head);
    assert((MEM32(hdr + 16) & 0x13u) == 3 && (MEM32(hdr2 + 16) & 0x13u) == 3);

    assert(waveOutWrite_impl() == 0 && waveOutReset_impl() == 0);
    assert(!w->head && (MEM32(hdr2 + 16) & 0x13u) == 3);
    assert(waveOutClose_impl() == 0 && !w->active);

    uint8_t fresh[4] = {1,1,1,1};
    dsbuf_t ds = { .com = { .cls = &buffer_class }, .data = 0x5000, .size = 8, .channels = 1,
        .bits = 16, .block = 2, .frequency = 24000, .rate = 24000, .playing = 1, .looping = 1,
        .fresh = fresh, .last_consumed = 0xFFFFFFFFu };
    buffers = test_ds = &ds;
    float receipt_mix[16];
    mix_chunk(receipt_mix, 8);
    assert(ds.source_frames == 4 && ds.stale_frames == 0); /* upsampling repeats are valid */
    mix_chunk(receipt_mix, 8);
    assert(ds.source_frames == 8 && ds.stale_frames == 4 && ds.max_stale_run == 4);
    g_esp = 0x1000; memset(GPTR(g_esp), 0, 40); ARG(1) = ds.data; ARG(2) = ds.size;
    assert(dsb_Unlock_impl() == 0 && fresh[0] && fresh[3]);
    mix_chunk(receipt_mix, 8);
    assert(ds.source_frames == 12 && ds.stale_frames == 4 && ds.stale_run == 0);

    ds.size = 16384; ds.rate = ds.frequency = 48000; ds.play_pos = 8;
    memset(GPTR(g_esp), 0, 40); ARG(2) = 4; ARG(3) = 0x6000; ARG(4) = 0x6004;
    ARG(5) = 0x6008; ARG(6) = 0x600C; ARG(7) = 1;
    assert(dsb_Lock_impl() == 0 && MEM32(0x6000) == ds.data + 968 && MEM32(0x6004) == 4);
    ARG(1) = 0x6000; ARG(2) = 0x6004;
    assert(dsb_GetCurrentPosition_impl() == 0 && MEM32(0x6000) == 8 && MEM32(0x6004) == 968);
    ds.size = 512; assert(buffer_write_cursor(&ds) == 264);
    ds.playing = 0; assert(buffer_write_cursor(&ds) == 8);
    buffers = test_ds = NULL;
    g_cur_func = 0x21101300; calibrate_miles_lookahead();
    assert(test_preference == 24 && preference_writes == 1);
    calibrate_miles_lookahead(); assert(preference_writes == 1);
    test_preference = 12; calibrate_miles_lookahead(); assert(test_preference == 12 && preference_writes == 1);
    g_cur_func = 0x30001000; test_preference = 8; calibrate_miles_lookahead();
    assert(test_preference == 8 && preference_writes == 1);
    puts("winmm_selfcheck: PCM/queues, DS freshness/writecursor, Miles lookahead calibration with explicit preference preservation PASS");
    return 0;
}
