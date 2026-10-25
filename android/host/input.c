/* Input state: what Android touch controls (or a test script) produce, kept
 * as Win32 virtual keys, DirectInput scan codes, a relative mouse and an
 * absolute cursor. Host threads record events; guest message APIs and
 * DirectInput read them under the machine lock. */
#include "host.h"
#include "input.h"

static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static int screen_w = 1280, screen_h = 720;
static int cursor_x = 640, cursor_y = 360;
static uint8_t vk_down[256], vk_toggle[256];
static uint8_t dik_down[256];
/* Pressed since the last snapshot: a tap shorter than a frame (a touch
 * button, a slow map frame) is still seen once by a game that polls with
 * GetDeviceState, as GetAsyncKeyState's "pressed since" bit does on Windows. */
static uint8_t dik_latched[256], mouse_latched[8];
static int32_t mouse_dx, mouse_dy, mouse_dz;
static uint8_t mouse_buttons[8];
static uint32_t sequence;
static int pointer_edges = -1;

#define RING 256
typedef struct { void_input_event e[RING]; int head, count; } ring_t;
static ring_t key_ring, mouse_ring;

typedef struct { uint32_t msg, wparam, lparam; } pending_t;
static pending_t pending[512];
static int pending_count;

void void_input_screen_size(int *w, int *h) { *w = screen_w; *h = screen_h; }
void void_input_set_screen_size(int w, int h)
{
    pthread_mutex_lock(&input_lock);
    screen_w = w; screen_h = h;
    if (cursor_x >= w) cursor_x = w / 2;
    if (cursor_y >= h) cursor_y = h / 2;
    pthread_mutex_unlock(&input_lock);
}

void void_input_cursor(uint32_t *x, uint32_t *y)
{
    pthread_mutex_lock(&input_lock);
    *x = (uint32_t)cursor_x; *y = (uint32_t)cursor_y;
    pthread_mutex_unlock(&input_lock);
}

void void_input_set_cursor(int x, int y)
{
    pthread_mutex_lock(&input_lock);
    cursor_x = x < 0 ? 0 : x >= screen_w ? screen_w - 1 : x;
    cursor_y = y < 0 ? 0 : y >= screen_h ? screen_h - 1 : y;
    pthread_mutex_unlock(&input_lock);
}

int16_t void_input_key_state(uint32_t vk)
{
    pthread_mutex_lock(&input_lock);
    int16_t s = (int16_t)((vk_down[vk & 0xFF] ? 0x8000 : 0) | (vk_toggle[vk & 0xFF] & 1));
    pthread_mutex_unlock(&input_lock);
    return s;
}

uint32_t void_input_vk_char(uint32_t vk)
{
    int shift = vk_down[0x10] != 0;
    if (vk >= 'A' && vk <= 'Z') return shift ? vk : vk + 32;
    if (vk >= '0' && vk <= '9') return vk;
    switch (vk) {
    case 0x20: return ' ';
    case 0x0D: return '\r';
    case 0x08: return 8;
    case 0x1B: return 27;
    case 0x09: return '\t';
    case 0xBD: return shift ? '_' : '-';
    case 0xBB: return shift ? '+' : '=';
    case 0xBE: return shift ? '>' : '.';
    case 0xBC: return shift ? '<' : ',';
    case 0xBF: return shift ? '?' : '/';
    case 0xBA: return shift ? ':' : ';';
    case 0xDE: return shift ? '"' : '\'';
    case 0xC0: return shift ? '~' : '`';
    default: return 0;
    }
}

/* Virtual key -> DirectInput scan code (DIK_*). */
uint32_t void_vk_to_dik(uint32_t vk)
{
    static const uint8_t letters[26] = {
        0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
        0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    static const uint8_t digits[10] = { 0x0B,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A };
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= '0' && vk <= '9') return digits[vk - '0'];
    if (vk >= 0x70 && vk <= 0x79) return 0x3B + (vk - 0x70);   /* F1..F10 */
    switch (vk) {
    case 0x7A: return 0x57;   /* F11 */
    case 0x7B: return 0x58;   /* F12 */
    case 0x1B: return 0x01;   /* ESCAPE */
    case 0x0D: return 0x1C;   /* RETURN */
    case 0x20: return 0x39;   /* SPACE */
    case 0x09: return 0x0F;   /* TAB */
    case 0x08: return 0x0E;   /* BACK */
    case 0x10: case 0xA0: return 0x2A;   /* LSHIFT */
    case 0xA1: return 0x36;   /* RSHIFT */
    case 0x11: case 0xA2: return 0x1D;   /* LCONTROL */
    case 0xA3: return 0x9D;   /* RCONTROL */
    case 0x12: case 0xA4: return 0x38;   /* LMENU */
    case 0xA5: return 0xB8;   /* RMENU */
    case 0x25: return 0xCB;   /* LEFT */
    case 0x26: return 0xC8;   /* UP */
    case 0x27: return 0xCD;   /* RIGHT */
    case 0x28: return 0xD0;   /* DOWN */
    case 0x21: return 0xC9;   /* PRIOR (Page Up) */
    case 0x22: return 0xD1;   /* NEXT (Page Down) */
    case 0x24: return 0xC7;   /* HOME */
    case 0x23: return 0xCF;   /* END */
    case 0x2D: return 0xD2;   /* INSERT */
    case 0x2E: return 0xD3;   /* DELETE */
    case 0xBD: return 0x0C;   /* MINUS */
    case 0xBB: return 0x0D;   /* EQUALS */
    case 0xC0: return 0x29;   /* GRAVE */
    case 0xBA: return 0x27;   /* SEMICOLON */
    case 0xDE: return 0x28;   /* APOSTROPHE */
    case 0xBC: return 0x33;   /* COMMA */
    case 0xBE: return 0x34;   /* PERIOD */
    case 0xBF: return 0x35;   /* SLASH */
    case 0xDB: return 0x1A;   /* LBRACKET */
    case 0xDD: return 0x1B;   /* RBRACKET */
    case 0xDC: return 0x2B;   /* BACKSLASH */
    case 0x13: return 0xC5;   /* PAUSE */
    case 0x6B: return 0x4E;   /* NUMPAD ADD */
    case 0x6D: return 0x4A;   /* NUMPAD SUBTRACT */
    /* The rest of a physical keyboard's numeric pad and its locks. */
    case 0x60: return 0x52;   /* NUMPAD0 */
    case 0x61: return 0x4F;   /* NUMPAD1 */
    case 0x62: return 0x50;   /* NUMPAD2 */
    case 0x63: return 0x51;   /* NUMPAD3 */
    case 0x64: return 0x4B;   /* NUMPAD4 */
    case 0x65: return 0x4C;   /* NUMPAD5 */
    case 0x66: return 0x4D;   /* NUMPAD6 */
    case 0x67: return 0x47;   /* NUMPAD7 */
    case 0x68: return 0x48;   /* NUMPAD8 */
    case 0x69: return 0x49;   /* NUMPAD9 */
    case 0x6A: return 0x37;   /* MULTIPLY */
    case 0x6E: return 0x53;   /* DECIMAL */
    case 0x6F: return 0xB5;   /* DIVIDE */
    case 0x14: return 0x3A;   /* CAPITAL */
    case 0x90: return 0x45;   /* NUMLOCK */
    case 0x91: return 0x46;   /* SCROLL */
    default: return 0;
    }
}

static void ring_push(ring_t *r, uint32_t ofs, uint32_t data)
{
    void_input_event *e = &r->e[(r->head + r->count) % RING];
    if (r->count == RING) { r->head = (r->head + 1) % RING; --r->count; e = &r->e[(r->head + r->count) % RING]; }
    e->ofs = ofs; e->data = data; e->time = void_tick_ms(); e->seq = ++sequence;
    ++r->count;
}

static int ring_take(ring_t *r, void_input_event *out, int max, int peek)
{
    pthread_mutex_lock(&input_lock);
    int n = r->count < max ? r->count : max;
    for (int i = 0; i < n; ++i) out[i] = r->e[(r->head + i) % RING];
    if (!peek) { r->head = (r->head + n) % RING; r->count -= n; }
    pthread_mutex_unlock(&input_lock);
    return n;
}

static void pend(uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    if (pending_count < 512) { pending[pending_count].msg = msg; pending[pending_count].wparam = wparam; pending[pending_count].lparam = lparam; ++pending_count; }
}

int void_input_text(const uint16_t *text, uint32_t length)
{
    if ((!text && length) || length > VOID_INPUT_TEXT_MAX) return 0;
    for (uint32_t i = 0; i < length; ++i) {
        uint16_t c = text[i];
        if (c < 0x20 || c == 0x7F) return 0;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (++i >= length || text[i] < 0xDC00 || text[i] > 0xDFFF) return 0;
        } else if (c >= 0xDC00 && c <= 0xDFFF) return 0;
    }
    pthread_mutex_lock(&input_lock);
    int available = length <= 512u - (uint32_t)pending_count;
    if (available) for (uint32_t i = 0; i < length; ++i) pend(0x0102u, text[i], 1u);
    pthread_mutex_unlock(&input_lock);
    return available;
}

/* A modifier arrives as its generic VK (VK_CONTROL from a scan code or a
 * script) or a sided one; a keyboard's key is a sided key. Windows keeps the
 * sided and the generic state (GetAsyncKeyState answers both) and posts the
 * generic VK in WM_KEYDOWN/UP. */
void void_input_key(uint32_t vk, int down)
{
    vk &= 0xFF;
    if (vk >= 0x10 && vk <= 0x12) vk = 0xA0 + 2 * (vk - 0x10);   /* SHIFT, CONTROL, MENU -> left */
    uint32_t message_vk = vk >= 0xA0 && vk <= 0xA5 ? 0x10 + (vk - 0xA0) / 2 : vk;
    pthread_mutex_lock(&input_lock);
    int was = vk_down[vk];
    vk_down[vk] = (uint8_t)(down != 0);
    if (down && !was) vk_toggle[vk] ^= 1;
    uint32_t dik = void_vk_to_dik(vk);
    if (dik) {
        dik_down[dik] = down ? 0x80 : 0;
        if (down) dik_latched[dik] = 0x80;
        ring_push(&key_ring, dik, down ? 0x80u : 0u);
    }
    if (vk == 0xA0 || vk == 0xA1) vk_down[0x10] = vk_down[0xA0] | vk_down[0xA1];
    if (vk == 0xA2 || vk == 0xA3) vk_down[0x11] = vk_down[0xA2] | vk_down[0xA3];
    if (vk == 0xA4 || vk == 0xA5) vk_down[0x12] = vk_down[0xA4] | vk_down[0xA5];
    uint32_t lparam = 1u | (dik << 16) | (down ? 0u : 0xC0000000u) | (was && down ? 0x40000000u : 0u);
    pend(down ? 0x0100u : 0x0101u, message_vk, lparam);
    pthread_mutex_unlock(&input_lock);
}

void void_input_mouse_move(int dx, int dy)
{
    pthread_mutex_lock(&input_lock);
    mouse_dx += dx; mouse_dy += dy;
    cursor_x += dx; cursor_y += dy;
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_x >= screen_w) cursor_x = screen_w - 1;
    if (cursor_y >= screen_h) cursor_y = screen_h - 1;
    if (dx) ring_push(&mouse_ring, 0, (uint32_t)dx);     /* DIMOFS_X */
    if (dy) ring_push(&mouse_ring, 4, (uint32_t)dy);     /* DIMOFS_Y */
    pend(0x0200u, 0, ((uint32_t)(cursor_y & 0xFFFF) << 16) | (uint32_t)(cursor_x & 0xFFFF));
    pthread_mutex_unlock(&input_lock);
}

void void_input_mouse_warp(int x, int y)
{
    pthread_mutex_lock(&input_lock);
    cursor_x = x < 0 ? 0 : x >= screen_w ? screen_w - 1 : x;
    cursor_y = y < 0 ? 0 : y >= screen_h ? screen_h - 1 : y;
    pend(0x0200u, 0, ((uint32_t)(cursor_y & 0xFFFF) << 16) | (uint32_t)(cursor_x & 0xFFFF));
    pthread_mutex_unlock(&input_lock);
}

void void_input_mouse_button(int button, int down)
{
    if (button < 0 || button > 7) return;
    VLOG("[input] button %d %s at %d,%d (%.3f s)", button, down ? "down" : "up", cursor_x, cursor_y,
         (double)void_monotonic_ns() / 1e9);
    pthread_mutex_lock(&input_lock);
    mouse_buttons[button] = down ? 0x80 : 0;
    if (down) mouse_latched[button] = 0x80;
    static const uint8_t vks[3] = { 0x01, 0x02, 0x04 };
    if (button < 3) vk_down[vks[button]] = (uint8_t)(down != 0);
    ring_push(&mouse_ring, 12u + (uint32_t)button, down ? 0x80u : 0u);   /* DIMOFS_BUTTON0.. */
    static const uint32_t downs[3] = { 0x0201, 0x0204, 0x0207 }, ups[3] = { 0x0202, 0x0205, 0x0208 };
    if (button < 3) pend(down ? downs[button] : ups[button], 0, ((uint32_t)(cursor_y & 0xFFFF) << 16) | (uint32_t)(cursor_x & 0xFFFF));
    pthread_mutex_unlock(&input_lock);
}

void void_input_set_edges(int edges)
{
    pthread_mutex_lock(&input_lock);
    pointer_edges = edges;
    pthread_mutex_unlock(&input_lock);
}

int void_input_edges(void)
{
    pthread_mutex_lock(&input_lock);
    int edges = pointer_edges;
    pthread_mutex_unlock(&input_lock);
    return edges;
}

void void_input_mouse_wheel(int delta)
{
    pthread_mutex_lock(&input_lock);
    mouse_dz += delta;
    ring_push(&mouse_ring, 8, (uint32_t)delta);
    pend(0x020Au, (uint32_t)(delta & 0xFFFF) << 16, ((uint32_t)(cursor_y & 0xFFFF) << 16) | (uint32_t)(cursor_x & 0xFFFF));
    pthread_mutex_unlock(&input_lock);
}

void void_input_keyboard_snapshot(uint8_t keys[256])
{
    pthread_mutex_lock(&input_lock);
    for (int i = 0; i < 256; ++i) keys[i] = dik_down[i] | dik_latched[i];
    memset(dik_latched, 0, sizeof dik_latched);
    pthread_mutex_unlock(&input_lock);
}

void void_input_mouse_snapshot(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t buttons[8])
{
    pthread_mutex_lock(&input_lock);
    *dx = mouse_dx; *dy = mouse_dy; *dz = mouse_dz;
    mouse_dx = mouse_dy = mouse_dz = 0;
    for (int i = 0; i < 8; ++i) buttons[i] = mouse_buttons[i] | mouse_latched[i];
    memset(mouse_latched, 0, sizeof mouse_latched);
    pthread_mutex_unlock(&input_lock);
}

int void_input_keyboard_events(void_input_event *out, int max, int peek) { return ring_take(&key_ring, out, max, peek); }
int void_input_mouse_events(void_input_event *out, int max, int peek) { return ring_take(&mouse_ring, out, max, peek); }

void void_input_pump(void)
{
    pending_t local[512];
    int n;
    pthread_mutex_lock(&input_lock);
    n = pending_count;
    memcpy(local, pending, sizeof(pending_t) * (size_t)n);
    pending_count = 0;
    pthread_mutex_unlock(&input_lock);
    uint32_t hwnd = void_window_main();
    if (!hwnd) return;
    uint32_t void_window_focus(void);
    uint32_t focus = void_window_focus();
    for (int i = 0; i < n; ++i) {
        uint32_t target = local[i].msg == 0x0102u ? focus : hwnd;
        if (target) void_post_message(target, local[i].msg, local[i].wparam, local[i].lparam);
    }
}

#ifdef VOID_INPUT_SELFTEST
#include <assert.h>
int void_verbose;
void void_log(const char *fmt, ...) { (void)fmt; }
uint32_t void_tick_ms(void) { return 0; }
uint64_t void_monotonic_ns(void) { return 0; }
static uint32_t test_focus = 0x20010;
uint32_t void_window_main(void) { return 0x10010; }
uint32_t void_window_focus(void) { return test_focus; }
static struct { uint32_t hwnd, msg, value; } posted[1024];
static unsigned posted_count;
void void_post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    (void)lparam;
    assert(posted_count < 1024);
    posted[posted_count].hwnd = hwnd; posted[posted_count].msg = msg; posted[posted_count++].value = wparam;
}
int main(void)
{
    const uint16_t text[] = { 'A', 0x044F, 0xD83D, 0xDE80 };
    assert(void_input_text(text, 4));
    void_input_pump();
    assert(posted_count == 4);
    for (unsigned i = 0; i < 4; ++i)
        assert(posted[i].hwnd == test_focus && posted[i].msg == 0x102 && posted[i].value == text[i]);
    assert(!void_input_text(text + 2, 1) && !void_input_text(text + 3, 1));
    const uint16_t newline[] = { '\r' };
    assert(!void_input_text(newline, 1) && !pending_count);
    uint16_t full[128];
    for (unsigned i = 0; i < 128; ++i) full[i] = 'x';
    for (int i = 0; i < 4; ++i) assert(void_input_text(full, 128));
    assert(pending_count == 512 && !void_input_text(text, 4) && pending_count == 512);
    void_input_pump();
    assert(posted_count == 516);
    test_focus = 0;
    assert(void_input_text(text, 4));
    void_input_pump();
    assert(posted_count == 516);
    assert(void_vk_to_dik(8) == 0x0E && void_vk_to_dik(13) == 0x1C);
    void_input_key(8, 1); void_input_key(8, 1); void_input_key(8, 0);
    void_input_pump();
    assert(posted_count == 519 && posted[516].msg == 0x100 && posted[517].value == 8 && posted[518].msg == 0x101);
    /* Every scan code a physical keyboard sends has exactly one virtual key. */
    for (uint32_t vk = 1; vk < 256; ++vk) for (uint32_t other = vk + 1; other < 256; ++other)
        assert(!void_vk_to_dik(vk) || void_vk_to_dik(vk) != void_vk_to_dik(other) ||
               (vk >= 0x10 && vk <= 0x12 && other >= 0xA0 && other <= 0xA5));
    assert(void_vk_to_dik(0x60) == 0x52 && void_vk_to_dik(0x69) == 0x49 && void_vk_to_dik(0x6F) == 0xB5);
    puts("input UTF-16, surrogate pairs, atomic queue, focus, Backspace and keypad scan codes: PASS");
    return 0;
}
#endif
