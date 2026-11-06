/* Guest strings: UTF-16LE and the Russian installation's Windows-1251 ACP. */
#include "host.h"

/* Windows-1252 0x80..0x9F; 0 marks the five undefined bytes, which
 * MultiByteToWideChar maps to the same-valued C1 control. */
static const uint16_t cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

uint16_t void_cp1252_to_u16(uint8_t c)
{
    return (c >= 0x80 && c < 0xA0) ? cp1252_high[c - 0x80] : c;
}

int void_u16_to_cp1252(uint16_t u, uint8_t *out)
{
    if (u < 0x80 || (u >= 0xA0 && u < 0x100)) { *out = (uint8_t)u; return 1; }
    for (int i = 0; i < 32; ++i) if (cp1252_high[i] == u) { *out = (uint8_t)(0x80 + i); return 1; }
    *out = '?';
    return 0;
}

/* Explicit Central European requests come from the Polish TEX loader. */
static const uint16_t cp1250_high[128] = {
    0x20AC,0x0081,0x201A,0x0083,0x201E,0x2026,0x2020,0x2021,0x0088,0x2030,0x0160,0x2039,0x015A,0x0164,0x017D,0x0179,
    0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x0098,0x2122,0x0161,0x203A,0x015B,0x0165,0x017E,0x017A,
    0x00A0,0x02C7,0x02D8,0x0141,0x00A4,0x0104,0x00A6,0x00A7,0x00A8,0x00A9,0x015E,0x00AB,0x00AC,0x00AD,0x00AE,0x017B,
    0x00B0,0x00B1,0x02DB,0x0142,0x00B4,0x00B5,0x00B6,0x00B7,0x00B8,0x0105,0x015F,0x00BB,0x013D,0x02DD,0x013E,0x017C,
    0x0154,0x00C1,0x00C2,0x0102,0x00C4,0x0139,0x0106,0x00C7,0x010C,0x00C9,0x0118,0x00CB,0x011A,0x00CD,0x00CE,0x010E,
    0x0110,0x0143,0x0147,0x00D3,0x00D4,0x0150,0x00D6,0x00D7,0x0158,0x016E,0x00DA,0x0170,0x00DC,0x00DD,0x0162,0x00DF,
    0x0155,0x00E1,0x00E2,0x0103,0x00E4,0x013A,0x0107,0x00E7,0x010D,0x00E9,0x0119,0x00EB,0x011B,0x00ED,0x00EE,0x010F,
    0x0111,0x0144,0x0148,0x00F3,0x00F4,0x0151,0x00F6,0x00F7,0x0159,0x016F,0x00FA,0x0171,0x00FC,0x00FD,0x0163,0x02D9,
};
uint16_t void_cp1250_to_u16(uint8_t c) { return c < 0x80 ? c : cp1250_high[c - 0x80]; }
int void_u16_to_cp1250(uint16_t u, uint8_t *out)
{
    if (u < 0x80) { *out = (uint8_t)u; return 1; }
    for (int i=0; i<128; i++) if (cp1250_high[i] == u) { *out = (uint8_t)(0x80+i); return 1; }
    *out = '?'; return 0;
}

static const uint16_t cp1251_high[128] = {
    0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
    0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x0098,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
    0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
    0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,
    0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
    0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
    0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
    0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F,
};

uint16_t void_cp1251_to_u16(uint8_t c) { return c < 0x80 ? c : cp1251_high[c - 0x80]; }

int void_u16_to_cp1251(uint16_t u, uint8_t *out)
{
    if (u < 0x80) { *out = (uint8_t)u; return 1; }
    if (u >= 0x0410 && u <= 0x044F) { *out = (uint8_t)(u - 0x0410 + 0xC0); return 1; }
    for (int i = 0; i < 64; ++i) if (cp1251_high[i] == u) { *out = (uint8_t)(0x80 + i); return 1; }
    *out = '?';
    return 0;
}

size_t void_utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode one UTF-8 code point; invalid bytes decode as U+FFFD. */
static uint32_t utf8_next(const unsigned char **p)
{
    const unsigned char *s = *p;
    uint32_t c = *s++;
    if (c >= 0x80) {
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
        if (extra < 0) { *p = s; return 0xFFFD; }
        c &= 0x3Fu >> extra;
        for (int i = 0; i < extra; ++i) {
            if ((*s & 0xC0) != 0x80) { *p = s; return 0xFFFD; }
            c = (c << 6) | (*s++ & 0x3F);
        }
    }
    *p = s;
    return c;
}

size_t void_gstrlen(uint32_t va)
{
    return va ? strlen((const char *)GPTR(va)) : 0;
}

size_t void_gwcslen(uint32_t va)
{
    size_t n = 0;
    if (!va) return 0;
    while (MEM16(va + 2u * (uint32_t)n)) ++n;
    return n;
}

char *void_wstr_utf8(uint32_t va)
{
    if (!va) return NULL;
    size_t n = void_gwcslen(va);
    char *out = (char *)malloc(n * 3 + 1), *w = out;
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = MEM16(va + 2u * (uint32_t)i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            uint32_t low = MEM16(va + 2u * (uint32_t)(i + 1));
            if (low >= 0xDC00 && low < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00); ++i; }
        }
        w += void_utf8_encode(c, w);
    }
    *w = 0;
    return out;
}

char *void_astr_utf8(uint32_t va)
{
    if (!va) return NULL;
    size_t n = void_gstrlen(va);
    const uint8_t *s = (const uint8_t *)GPTR(va);
    char *out = (char *)malloc(n * 3 + 1), *w = out;
    for (size_t i = 0; i < n; ++i) w += void_utf8_encode(void_cp1251_to_u16(s[i]), w);
    *w = 0;
    return out;
}

uint32_t void_utf8_to_gw(const char *s, uint32_t dst, uint32_t cap)
{
    uint32_t need = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) { uint32_t c = utf8_next(&p); need += c >= 0x10000 ? 2 : 1; }
    if (need + 1 > cap) return need + 1;
    uint32_t i = 0;
    p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = utf8_next(&p);
        if (c >= 0x10000) {
            c -= 0x10000;
            MEM16(dst + 2 * i++) = (uint16_t)(0xD800 + (c >> 10));
            MEM16(dst + 2 * i++) = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            MEM16(dst + 2 * i++) = (uint16_t)c;
        }
    }
    MEM16(dst + 2 * i) = 0;
    return i;
}

uint32_t void_utf8_to_ga(const char *s, uint32_t dst, uint32_t cap)
{
    uint32_t need = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) { (void)utf8_next(&p); ++need; }
    if (need + 1 > cap) return need + 1;
    uint32_t i = 0;
    p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = utf8_next(&p);
        uint8_t b;
        void_u16_to_cp1251(c > 0xFFFF ? 0xFFFD : (uint16_t)c, &b);
        MEM8(dst + i++) = b;
    }
    MEM8(dst + i) = 0;
    return i;
}
