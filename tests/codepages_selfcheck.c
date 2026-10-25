/* Fixed Russian-install ACP; explicit Western and UTF-8 requests stay explicit. */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include "../android/host/strings.c"
#include "../android/host/kernel32.c"
#include "../android/host/system.c"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;
int void_verbose;
static uint32_t last_error, heap_at = 0x8000;
void void_set_last_error(uint32_t error) { last_error = error; }
void void_log(const char *fmt, ...) { (void)fmt; }
int void_vm_readable(uint32_t p, uint32_t n) { return p >= 0x1000 && p < 0x10000 && n <= 0x10000u - p; }
int void_vm_writable(uint32_t p, uint32_t n) { return void_vm_readable(p, n); }
uint32_t void_galloc(uint32_t n) { uint32_t p = heap_at; heap_at += (n + 15) & ~15u; assert(heap_at < 0x10000); return p; }

static void args(uint32_t cp, uint32_t src, uint32_t n, uint32_t dst, uint32_t capacity)
{
    g_esp = 0x1000; memset(GPTR(g_esp), 0, 64);
    ARG(0) = cp; ARG(2) = src; ARG(3) = n; ARG(4) = dst; ARG(5) = capacity;
}

int main(void)
{
    assert(mmap((void *)(uintptr_t)VOID_GUEST_BASE, 0x10000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)VOID_GUEST_BASE);
    const char *player = "Лорд-крестоносец";
    const uint8_t encoded[] = { 0xCB,0xEE,0xF0,0xE4,0x2D,0xEA,0xF0,0xE5,0xF1,0xF2,0xEE,0xED,0xEE,0xF1,0xE5,0xF6,0 };
    assert(GetACP_impl() == 1251 && real_cp(0) == 1251 && real_cp(3) == 1251);
    for (int i = 0; i < 256; ++i) {
        uint8_t b;
        assert(void_u16_to_cp1251(void_cp1251_to_u16((uint8_t)i), &b) && b == i);
    }
    assert(void_utf8_to_gw(player, 0x3000, 64) == 16);
    assert(void_utf8_to_ga(player, 0, 0) == 17);
    MEM8(0x4000) = 0x55;
    assert(void_utf8_to_ga(player, 0x4000, 16) == 17 && MEM8(0x4000) == 0x55);
    assert(void_utf8_to_ga(player, 0x4000, 17) == 16 && !memcmp(GPTR(0x4000), encoded, 17));
    char *decoded = void_astr_utf8(0x4000); assert(!strcmp(decoded, player)); free(decoded);
    uint16_t *gdi = gdi_text(0x4000, 16, 0); assert(gdi && !memcmp(gdi, GPTR(0x3000), 32)); free(gdi);

    for (uint32_t cp = 0; cp <= 3; cp += 3) {
        args(cp, 0x3000, 0xFFFFFFFFu, 0, 0);
        assert(WideCharToMultiByte_impl() == 17);
        ARG(4) = 0x4000; ARG(5) = 17; ARG(7) = 0x5000;
        assert(WideCharToMultiByte_impl() == 17 && MEM32(0x5000) == 0 && !memcmp(GPTR(0x4000), encoded, 17));
        args(cp, 0x4000, 0xFFFFFFFFu, 0x6000, 64);
        assert(MultiByteToWideChar_impl() == 17 && !memcmp(GPTR(0x6000), GPTR(0x3000), 34));
    }
    args(1251, 0x3000, 16, 0x4000, 15);
    MEM8(0x400F) = 0x55;
    assert(WideCharToMultiByte_impl() == 0 && last_error == ERROR_INSUFFICIENT_BUFFER_ && MEM8(0x400F) == 0x55);
    args(0, 0x4000, 16, 0x6000, 15); MEM16(0x601E) = 0x5555;
    assert(MultiByteToWideChar_impl() == 0 && last_error == ERROR_INSUFFICIENT_BUFFER_ && MEM16(0x601E) == 0x5555);

    /* Explicit 1252 still means Western text; it is never redirected to ACP. */
    MEM16(0x3100) = 0x00E9; MEM16(0x3102) = 0x20AC; MEM16(0x3104) = 0x0152; MEM16(0x3106) = 0;
    const uint8_t western[] = {0xE9,0x80,0x8C,0};
    args(1252, 0x3100, 0xFFFFFFFFu, 0x4100, 16); ARG(7) = 0x5000;
    assert(WideCharToMultiByte_impl() == 4 && !memcmp(GPTR(0x4100), western, 4) && MEM32(0x5000) == 0);
    args(1252, 0x4100, 4, 0x6100, 16);
    assert(MultiByteToWideChar_impl() == 4 && !memcmp(GPTR(0x6100), GPTR(0x3100), 8));
    MEM8(0x5100) = '!'; args(1252, 0x3000, 16, 0x4100, 32); ARG(6) = 0x5100; ARG(7) = 0x5000;
    assert(WideCharToMultiByte_impl() == 16 && MEM8(0x4100) == '!' && MEM8(0x4104) == '-' && MEM32(0x5000) == 1);

    /* UTF-8 round-trips a surrogate pair and respects exact output capacities. */
    MEM16(0x3200) = 0x042F; MEM16(0x3202) = 0xD83D; MEM16(0x3204) = 0xDE00; MEM16(0x3206) = 0;
    const uint8_t utf8[] = {0xD0,0xAF,0xF0,0x9F,0x98,0x80,0};
    args(65001, 0x3200, 0xFFFFFFFFu, 0, 0); assert(WideCharToMultiByte_impl() == 7);
    ARG(4) = 0x4200; ARG(5) = 7; assert(WideCharToMultiByte_impl() == 7 && !memcmp(GPTR(0x4200), utf8, 7));
    args(65001, 0x4200, 7, 0x6200, 4); assert(MultiByteToWideChar_impl() == 4 && !memcmp(GPTR(0x6200), GPTR(0x3200), 8));
    args(65001, 0x3200, 0xFFFFFFFFu, 0x4200, 5); MEM8(0x4205) = 0x55;
    assert(WideCharToMultiByte_impl() == 0 && MEM8(0x4205) == 0x55);
    args(0, 0x3000, 0, 0x4000, 32); assert(WideCharToMultiByte_impl() == 0 && last_error == ERROR_INVALID_PARAMETER_);
    args(0, 0x4000, (uint32_t)-2, 0x6000, 32); assert(MultiByteToWideChar_impl() == 0 && last_error == ERROR_INVALID_PARAMETER_);
    args(0, 0, 0, 0, 0); ARG(1) = 0x5200; assert(GetCPInfo_impl() == 1 && MEM32(0x5200) == 1 && MEM8(0x5204) == '?');
    ARG(0) = 65001; assert(GetCPInfo_impl() == 1 && MEM32(0x5200) == 4);
    ARG(0) = 99999; assert(GetCPInfo_impl() == 0 && last_error == ERROR_INVALID_PARAMETER_);

    const char *path = "C:\\Users\\Player\\Documents\\Stronghold Crusader\\SavesExtreme\\Лорд-крестоносец.sav";
    uint32_t path_n = void_utf8_to_ga(path, 0x4300, 256); assert(path_n < 256);
    char *host_path = read_units(0x4300, 1); assert(!strcmp(host_path, path));
    assert(write_units(host_path, 0x4400, 256, 1) == path_n && !memcmp(GPTR(0x4400), GPTR(0x4300), path_n + 1)); free(host_path);

    strcpy((char *)GPTR(0x4500), "PLAYER_TEST"); memcpy(GPTR(0x4600), encoded, 17);
    g_esp = 0x1000; ARG(0) = 0x4500; ARG(1) = 0x4600; assert(SetEnvironmentVariableA_impl() == 1);
    ARG(1) = 0; ARG(2) = 0; assert(GetEnvironmentVariableA_impl() == 17);
    ARG(1) = 0x4700; ARG(2) = 17; assert(GetEnvironmentVariableA_impl() == 16 && !memcmp(GPTR(0x4700), encoded, 17));
    uint32_t block = env_block(0); const char *entry = (const char *)GPTR(block);
    while (*entry && strncmp(entry, "PLAYER_TEST=", 12)) entry += strlen(entry) + 1;
    assert(*entry && !memcmp(entry + 12, encoded, 17));
    puts("codepages_selfcheck: Russian ACP/default-player, all CP1251 bytes, ANSI GDI/path/env, explicit1252/UTF8 boundaries PASS");
    return 0;
}
