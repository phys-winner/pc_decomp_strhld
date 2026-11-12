#ifndef STRONGHOLD_GAME_PROFILE_H
#define STRONGHOLD_GAME_PROFILE_H
#include "host.h"
#include "edition.h"
#include <string.h>

/* Only persistent campaign fields from the original config writer, not live match timers. */
#ifdef STRONGHOLD_ORIGINAL
#define PROFILE_GAME 0x1503D30u
#define PROFILE_NAME (0xEC64F0u + 336)
#define PROFILE_SAVE 0x47F2C0u
static const struct { uint32_t offset, size; } profile_fields[] = {
    {0x3c, 8}, {0x48, 4}, {0xc8, 24}
};
#else
#ifdef STRONGHOLD_CLASSIC
#define PROFILE_GAME 0x21259A0u
#define PROFILE_NAME (0x17893D0u + 336)
#define PROFILE_SAVE 0x496C20u
#else
#define PROFILE_GAME 0x2BB8E80u
#define PROFILE_NAME (0x20BC3F8u + 336)
#define PROFILE_SAVE 0x496DB0u
#endif
static const struct { uint32_t offset, size; } profile_fields[] = {
    {0x44, 20}, {0x5c, 4}, {0xe0, 24},
    {0x29b0, 204}, {0x2b50, 4}, {0x2b58, 124},
    {0x2cf8, 8}, {0x2d00, 120}, {0x2e9c, 4}
};
#endif
static uint8_t profile_progress[512];
static uint16_t profile_name[250];
static int profile_ready;

/* Called on the guest thread: Android can kill the process without the original Exit callback. */
static void stronghold_profile_frame(uint32_t state, int accept_name)
{
    if (state == 0 || state == 3 || state == 26 || state == 40 || state == 48 ||
        (state == 41 && !accept_name)) return;
    uint8_t progress[sizeof profile_progress];
    size_t size = 0;
    for (size_t i = 0; i < sizeof profile_fields / sizeof profile_fields[0]; ++i) {
        uint32_t address = PROFILE_GAME + profile_fields[i].offset, count = profile_fields[i].size;
        if (!void_vm_readable(address, count)) return;
        memcpy(progress + size, GPTR(address), count);
        size += count;
    }
    int dirty = profile_ready && memcmp(progress, profile_progress, size);
    uint16_t name[250] = {0};
    if (accept_name) {
        if (!void_vm_readable(PROFILE_NAME, sizeof name)) return;
        memcpy(name, GPTR(PROFILE_NAME), sizeof name); name[249] = 0;
        if (name[0] && memcmp(name, profile_name, sizeof name)) dirty = 1;
    }
    if (dirty) {
        void_regs saved; void_regs_save(&saved);
        void_call_guest(PROFILE_SAVE, 0, NULL, 0);
        void_regs_load(&saved);
        if (name[0]) memcpy(profile_name, name, sizeof name);
        void_log("[profile] saved player name/campaign progress");
    }
    if (!profile_ready || dirty) memcpy(profile_progress, progress, size);
    profile_ready = 1;
}
#endif
