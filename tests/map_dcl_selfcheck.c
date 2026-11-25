#include <assert.h>
#include <sys/mman.h>
#include "../android/host/map_dcl.c"

uint32_t g_eax, g_ecx, g_esp;

int void_vm_readable(uint32_t at, uint32_t size)
{
    return at >= 0x1000 && (uint64_t)at + size <= 0x10000;
}

int void_vm_writable(uint32_t at, uint32_t size) { return void_vm_readable(at, size); }

static uint32_t decode_fixture(uint32_t packed, uint32_t target, uint32_t capacity)
{
    g_esp = 0x1000;
    ARG(0) = 0x2000;
    ARG(1) = 0x3000;
    ARG(2) = packed;
    ARG(3) = target;
    ARG(4) = capacity;
    stronghold_map_decode();
    assert(g_esp == 0x1018);
    return g_eax;
}

static void encode_args(unsigned mode, unsigned dictionary, uint32_t source, uint32_t target, uint32_t size)
{
    g_ecx = 0x2020;
    MEM32(g_ecx) = mode;
    MEM32(g_ecx + 4) = dictionary;
    g_esp = 0x1000;
    ARG(0) = 0x2008;
    ARG(1) = 0x200c;
    ARG(2) = source;
    ARG(3) = target;
    ARG(4) = size;
}

int main(void)
{
    assert(mmap(GPTR(0x1000), 0xf000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == GPTR(0x1000));
    const uint8_t compressed[] = {0x00, 0x04, 0x82, 0x24, 0x25, 0x8f, 0x80, 0x7f};
    const uint8_t expected[] = "AIAIAIAIAIAIA";
    memcpy(GPTR(0x3000), compressed, sizeof compressed);
    assert(decode_fixture(sizeof compressed, 0x4000, sizeof expected - 1) == 1);
    assert(!memcmp(GPTR(0x4000), expected, sizeof expected - 1));
    assert(MEM32(0x2000) == crc32(0, expected, sizeof expected - 1));
    MEM32(0x2000) = 42;
    assert(decode_fixture(sizeof compressed - 1, 0x4000, 128) == 0);
    assert(decode_fixture(sizeof compressed, 0x4000, 12) == 0);
    assert(decode_fixture(sizeof compressed, 0x3004, 32) == 0);
    assert(decode_fixture(sizeof compressed, 0x10000, 32) == 0);
    assert(MEM32(0x2000) == 42);
    assert(!memcmp(GPTR(0x3000), compressed, sizeof compressed));

    const uint32_t size = 8192;
    for (unsigned i = 0; i < size; ++i) MEM8(0x5000 + i) = (uint8_t)((i % 41) ^ ((i >> 9) & 1));
    for (unsigned mode = 0; mode <= 1; ++mode) {
        for (unsigned dict = 1024; dict <= 4096; dict *= 2) {
            encode_args(mode, dict, 0x5000, 0x3000, size);
            assert(stronghold_map_encode() && g_eax == 1 && g_esp == 0x1018);
            uint32_t packed = MEM32(0x200c), crc = MEM32(0x2008);
            assert(packed > 4 && packed < 0x2000 && MEM8(0x3000) == mode);
            assert(decode_fixture(packed, 0xa000, size) == 1);
            assert(crc == MEM32(0x2000) && crc == crc32(0, GPTR(0x5000), size));
            assert(!memcmp(GPTR(0x5000), GPTR(0xa000), size));
        }
    }
    uint32_t last_size = MEM32(0x200c), last_crc = MEM32(0x2008);
    MEM8(0x3000) = 0xab;
    encode_args(0, 4096, 0x5000, 0x3000, 1);
    assert(stronghold_map_encode() && g_eax == 0 && g_esp == 0x1018);
    assert(MEM8(0x3000) == 0xab && MEM32(0x200c) == last_size && MEM32(0x2008) == last_crc);
    encode_args(0, 4096, 0x5000, 0x5001, 32);
    assert(!stronghold_map_encode() && g_esp == 0x1000);
    puts("Native DCL encode/decode, CRC32, stdcall, truncation, destination size and overlap PASS");
}
