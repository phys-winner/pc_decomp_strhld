/* Standalone ARM64 check of the native Miles output ABI and PCM queue. */
#include <assert.h>
#include <sys/mman.h>
#include "../android/host/miles_audio.c"

uint32_t g_eax, g_ecx, g_edx, g_esp;
static unsigned allocation, renders;
static int compatibility_mode;
int void_audio_compatibility(void) { return compatibility_mode; }

int void_vm_readable(uint32_t p, uint32_t n) { return p >= 0x1000 && p <= 0x10000 && n <= 0x10000u - p; }
int void_vm_writable(uint32_t p, uint32_t n) { return void_vm_readable(p, n); }
uint32_t void_galloc(uint32_t n) { assert(n < 0x1000 && !allocation); allocation = n; return 0x7000; }
void void_gfree(uint32_t p) { assert(p == 0x7000 && allocation); allocation = 0; }
void void_log(const char *fmt, ...) { (void)fmt; }
_Noreturn void void_fatal(const char *fmt, ...) { fputs(fmt, stderr); abort(); }

uint32_t void_call_guest(uint32_t va, int nargs, const uint32_t *args, int pop)
{
    assert(va == 0x21112C90 && g_ecx == pcm.driver && g_edx == pcm.scratch);
    assert(nargs == 0 && !args && pop == 1);
    ++renders;
    uint8_t *samples = GPTR(pcm.scratch);
    memset(samples, 128, allocation);
    samples[0] = 128; samples[1] = 192; samples[2] = 0;
    return 0;
}

static void args(void)
{
    g_esp = 0x1000;
    g_ecx = 0x2000;
    g_edx = g_ecx + DIG_DEVICE;
    ARG(0) = UINT32_MAX;
    ARG(1) = 0x5000;
}

static void format(uint32_t rate, uint32_t channels, uint32_t bits)
{
    uint32_t addr = 0x5000, block = bits * channels / 8;
    MEM16(addr) = 1; MEM16(addr + 2) = channels;
    MEM32(addr + 4) = rate; MEM32(addr + 8) = rate * block;
    MEM16(addr + 12) = block; MEM16(addr + 14) = bits;
}

static void map_region(uint32_t offset, size_t size)
{
    void *target = GPTR(offset);
    assert(mmap(target, size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == target);
}

int main(void)
{
    map_region(0, 0x10000);
    map_region(0x2114C000, 0x1000);
    map_region(0x21150000, 0x10000);
    const int32_t accum[] = {INT32_MIN, -67108865, -67108864, -2049, -2048, -1,
                             0, 2047, 2048, 67106816, 67108863, 67108864, INT32_MAX};
    memcpy(GPTR(0x4001), accum, sizeof accum);
    for (unsigned wide = 0; wide <= 1; ++wide) {
        assert(stronghold_miles_copy_bulk(0x4001, 0x4203, sizeof accum / sizeof *accum, wide));
        for (unsigned i = 0; i < sizeof accum / sizeof *accum; ++i) {
            int32_t shifted = accum[i] >> 11;
            int expected = shifted < INT16_MIN ? INT16_MIN : shifted > INT16_MAX ? INT16_MAX : shifted;
            if (wide) assert((int16_t)MEM16(0x4203 + 2 * i) == expected);
            else assert(MEM8(0x4203 + i) == (expected >> 8) + 128);
        }
    }
    assert(!stronghold_miles_copy_bulk(0x4001, 0x4004, 8, 1));
    assert(!stronghold_miles_copy_bulk(0xFFF0, 0x4203, 8, 1));
    MEM32(0x2114C410) = 1234;
    MEM32(0x2114C414) = 678;
    for (unsigned i = 0; i < 64; ++i) MEM8(0x4001 + i) = (uint8_t)(17 * i + 65);
    for (unsigned mode = 0; mode < 16; ++mode) {
        unsigned stereo = mode & 1, wide = (mode >> 1) & 1, paired = (mode >> 2) & 1;
        unsigned lanes = 1 + stereo;
        uint32_t left = mode & 8 ? 1234u << (wide ? 0 : 8) : 1u << (wide ? 11 : 19);
        uint32_t right = mode & 8 ? 678u << (wide ? 0 : 8) : left;
        for (unsigned i = 0; i < 16 * lanes; ++i) MEM32(0x4400 + 4 * i) = UINT32_MAX - i;
        assert(stronghold_miles_merge_bulk(0x4001, 0x4400, 16, mode));
        for (unsigned i = 0; i < 16; ++i) {
            unsigned a = 0x4001 + i * (1 + paired) * (1 + wide), b = a + paired * (1 + wide);
            int32_t first = wide ? (int16_t)MEM16(a) : (int)MEM8(a) - 128;
            int32_t second = wide ? (int16_t)MEM16(b) : (int)MEM8(b) - 128;
            uint32_t mixed = (uint32_t)first * left, other = (uint32_t)second * right;
            unsigned at = i * lanes;
            assert(MEM32(0x4400 + 4 * at) == UINT32_MAX - at + mixed + (!stereo && paired ? other : 0));
            if (stereo) assert(MEM32(0x4404 + 4 * at) == UINT32_MAX - at - 1 + other);
        }
    }
    MEM32(0x2114C400) = 0x80000000u;
    MEM32(0x2114C408) = 1;
    MEM32(0x2114C404) = 2;
    MEM32(0x2114C418) = 0x4041;
    MEM32(0x2114C41C) = 0x4400 + 12 * 8;
    memset(GPTR(0x4400), 0, 12 * 8);
    uint32_t state[] = {0x4001, 0x4400, 0};
    assert(stronghold_miles_resample_bulk(state, 9, 16) == 11);
    assert(state[0] == 0x4011 && state[1] == 0x4400 + 11 * 8 && state[2] == 0x8000);
    for (unsigned i = 0; i < 11; ++i) {
        uint32_t sample = (uint32_t)((int)MEM8(0x4001 + i * 3 / 2) - 128);
        assert(MEM32(0x4400 + 8 * i) == sample * (1234u << 8));
        assert(MEM32(0x4404 + 8 * i) == sample * (678u << 8));
    }
    assert(!MEM32(0x4400 + 8 * 11) && !MEM32(0x4404 + 8 * 11));

    MEM32(MILES_FRAGMENT_MS) = 8;
    MEM32(MILES_LOOKAHEAD) = 8;
    MEM32(MILES_WAVE) = 1;

    args(); format(24000, 2, 24);
    sub_21111A50();
    assert(g_eax == 1 && g_esp == 0x1018 && !allocation);
    args(); format(24000, 1, 8);
    sub_21111A50();
    assert(!g_eax && g_esp == 0x1018 && MEM32(MILES_LOOKAHEAD) == 8 && !MEM32(MILES_WAVE));
    assert(pcm.driver == 0x2000 && pcm.fragment_frames == 192 && pcm.target == 1920);
    args(); sub_21112330();
    assert(g_esp == 0x1004 && MEM32(pcm.driver + DIG_PLAYING) == 1);

    /* Wrap publication and then resample a mono fragment into stereo. */
    pcm.head = FIFO_FRAMES - 1;
    output_running = 1;
    stronghold_audio_miles_fill(pcm.driver);
    assert(renders == 10 && pcm.count == pcm.target && g_ecx == 0x2000 && g_edx == 0x2068);
    int16_t stereo[16];
    read_pcm(stereo, 5);
    const int16_t expected[] = {0, 8192, 16384, -8192, -32768};
    for (unsigned i = 0; i < 5; ++i)
        assert(stereo[2 * i] == expected[i] && stereo[2 * i + 1] == expected[i]);
    assert(pcm.count == pcm.target - 2 && pcm.phase == 24000);
    stronghold_audio_miles_fill(pcm.driver);
    assert(renders == 11 && pcm.count == pcm.target + pcm.fragment_frames - 2);
    assert(MEM32(pcm.driver + DIG_RING_FRAGMENTS) == 9);

    /* Cover one 50 ms service period plus jitter, with a bounded response time. */
    int16_t chunk[2 * OUTPUT_CHUNK];
    for (unsigned i = 0; i < 7; ++i) read_pcm(chunk, OUTPUT_CHUNK);
    assert(!pcm.underrun_frames && pcm.count > 0);
    stronghold_audio_miles_fill(pcm.driver);
    assert(pcm.count >= pcm.target);
    assert(queued_pcm_frames() <= OUTPUT_RATE / 10);
    puts("70 ms without a Miles callback: no inserted silence, FIFO response below 100 ms");

    args(); ARG(0) = pcm.driver; sub_21114EC0();
    assert(g_eax == (queued_pcm_frames() + 47) / 48 && g_esp == 0x1008);
    args(); sub_21111EB0();
    assert(!g_eax && g_esp == 0x1004 && !pcm.count && !pcm.phase);
    memset(stereo, 99, sizeof stereo);
    read_pcm(stereo, 8);
    for (unsigned i = 0; i < 16; ++i) assert(stereo[i] == 0);
    args(); sub_21111E70();
    assert(!g_eax && g_esp == 0x1004 && !allocation && !pcm.driver);
    assert(MEM32(0x2000 + DIG_RELEASED) == 1 && !MEM32(0x2000 + DIG_OPEN));

    args(); format(44100, 2, 16);
    compatibility_mode = 1;
    MEM32(MILES_LOOKAHEAD) = 12;
    sub_21111A50();
    assert(!g_eax && MEM32(MILES_LOOKAHEAD) == 12 && pcm.fragment_frames == 356 && pcm.target == 12 * 356);
    int16_t *samples = GPTR(pcm.scratch);
    memset(samples, 0, allocation);
    samples[0] = 32767; samples[1] = -32768;
    publish_fragment();
    read_pcm(stereo, 1);
    assert(stereo[0] == 32767 && stereo[1] == -32768);
    args(); sub_21111E70();
    assert(!g_eax && !allocation);
    MEM32(MILES_LOOKAHEAD) = 8;
    args(); format(44100, 2, 16); sub_21111A50();
    assert(!g_eax && MEM32(MILES_LOOKAHEAD) == 8 && pcm.target == 13 * 356);
    args(); sub_21111E70(); assert(!allocation);
    puts("Compatibility reserve: 105 ms rounded to whole fragments; explicit lookahead retained PASS");
    puts("Miles output ABI, PCM8 mono, PCM16 stereo, fractional rate, FIFO wrap/reset and reopen PASS");
    return 0;
}
