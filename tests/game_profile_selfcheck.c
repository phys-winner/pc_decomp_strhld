#include "../android/host/game_profile.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

static int saves, stamp = 1234;
static uint32_t unreadable;
void void_log(const char *format, ...) { (void)format; }
int void_vm_readable(uint32_t address, uint32_t size)
{ return address != unreadable && address >= 0x400000 && (uint64_t)address + size <= 0x4000000; }
void void_regs_save(void_regs *r) { memset(r, 0, sizeof *r); r->eax = stamp; }
void void_regs_load(const void_regs *r) { stamp = r->eax; }
uint32_t void_call_guest(uint32_t fn, int count, const uint32_t *args, int pops)
{ assert(fn == PROFILE_SAVE && !count && !args && !pops); ++saves; stamp = 0; return 0; }

int main(void)
{
    assert(mmap(GPTR(0x400000), 0x3c00000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    const uint32_t startup[] = {0, 3, 26, 40, 48};
    MEM16(PROFILE_NAME) = 0x65E5; MEM16(PROFILE_NAME + 2) = 0x672C;
    for (size_t i = 0; i < sizeof startup / sizeof startup[0]; ++i) stronghold_profile_frame(startup[i], 0);
    stronghold_profile_frame(41, 0); assert(!saves && !profile_ready);
    stronghold_profile_frame(41, 1); assert(saves == 1 && stamp == 1234);
    stronghold_profile_frame(41, 1); assert(saves == 1);
    /* A battle/menu transition alone must not write the profile. */
    stronghold_profile_frame(14, 0); stronghold_profile_frame(37, 0); assert(saves == 1);
    size_t bytes = 0;
    for (size_t i = 0; i < sizeof profile_fields / sizeof profile_fields[0]; ++i) {
        uint32_t address = PROFILE_GAME + profile_fields[i].offset, count = profile_fields[i].size;
        bytes += count; assert(bytes <= sizeof profile_progress);
        printf("field %08X %u\n", address, count);
        /* Both ends matter: best times and the last unlocked stage must persist. */
        const uint32_t ends[] = {address, address + count - 1};
        for (size_t k = 0; k < 2; ++k) {
            int before = saves; ++MEM8(ends[k]);
            unreadable = address; stronghold_profile_frame(37, 0); assert(saves == before);
            unreadable = 0; stronghold_profile_frame(37, 0); assert(saves == before + 1 && stamp == 1234);
            stronghold_profile_frame(37, 0); assert(saves == before + 1);
        }
    }
#ifdef STRONGHOLD_ORIGINAL
    assert(bytes == 36);
#else
    assert(bytes == 512);
#endif
    int before = saves;
    ++MEM8(PROFILE_GAME + profile_fields[0].offset);
    stronghold_profile_frame(40, 0); stronghold_profile_frame(41, 0); assert(saves == before);
    stronghold_profile_frame(41, 1); assert(saves == before + 1);
    before = saves; MEM16(PROFILE_NAME + 4) = 'A';
    stronghold_profile_frame(14, 0); assert(saves == before);
    stronghold_profile_frame(41, 1); assert(saves == before + 1 && stamp == 1234);
    stronghold_profile_frame(41, 1); assert(saves == before + 1);
    puts("profile: accepted UTF16 name, every campaign/trail field, startup/dialog guards, unchanged frames and registers PASS");
    return 0;
}
