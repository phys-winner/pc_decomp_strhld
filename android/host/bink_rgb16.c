/* Scalar Bink 2x2 chroma conversion for the pinned 16-bit RGB paths.
 * Retain Bink's initialized lookup tables and exact packed word operations. */
#include "host.h"

static uint32_t rgb16(uint8_t y, uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t luma = MEM32(0x30054FF8 + 4u * y);
    return MEM32(0x300547A0 + luma + 4u * r) |
           MEM32(0x30052F70 + luma + 4u * g) |
           MEM32(0x30053B88 + luma + 4u * b);
}

static void coefficients(uint32_t u_at, uint32_t v_at, uint32_t *r, uint32_t *g, uint32_t *b)
{
    unsigned u = MEM8(u_at), v = MEM8(v_at);
    *r = MEM32(0x30055408 + u * 4);
    *g = MEM32(0x30055808 + v * 4) + MEM32(0x30055C08 + u * 4);
    *b = MEM32(0x30056008 + v * 4);
}

int stronghold_bink_rgb16_bulk(uint32_t groups, unsigned second, unsigned shifted)
{
    uint32_t source_slot = 0x30054FB8 + second * 4;
    uint32_t target_slot = 0x30054FB0 + second * 4;
    uint32_t y = MEM32(source_slot), dst = MEM32(target_slot);
    uint32_t u = MEM32(0x30054FC0), v = MEM32(0x30054FC4);
    uint32_t pixels = groups * 4, chroma = groups * 2;
    if (!groups || groups > 2048 || u < shifted || v < shifted ||
        !void_vm_readable(y, pixels) || !void_vm_writable(dst, pixels * 2) ||
        !void_vm_readable(u - shifted, chroma + shifted) ||
        !void_vm_readable(v - shifted, chroma + shifted))
        return 0;
    uint32_t r, g, b;
    if (shifted) coefficients(u - 1, v - 1, &r, &g, &b);
    for (uint32_t i = 0; i < groups; ++i) {
        if (shifted) {
            MEM16(dst) = (uint16_t)rgb16(MEM8(y), r, g, b);
            dst += 2;
            ++y;
        }
        for (unsigned pair = 0; pair < 2u - shifted; ++pair) {
            coefficients(u++, v++, &r, &g, &b);
            uint32_t low = rgb16(MEM8(y), r, g, b);
            uint32_t high = rgb16(MEM8(y + 1), r, g, b);
            MEM32(dst) = low | (high << 16);
            dst += 4;
            y += 2;
        }
        if (shifted) {
            coefficients(u++, v++, &r, &g, &b);
            MEM16(dst) = (uint16_t)rgb16(MEM8(y), r, g, b);
            dst += 2;
            ++y;
        }
    }
    MEM32(source_slot) = y;
    MEM32(target_slot) = dst;
    MEM32(0x30054FC0) = u;
    MEM32(0x30054FC4) = v;
    return 1;
}
