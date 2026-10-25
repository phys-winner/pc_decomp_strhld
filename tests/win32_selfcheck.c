/* ARM64 unit check with a deterministic text provider; real raster QA is in the APK. */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include "../android/host/strings.c"
#include "../android/host/kernel32.c"
#include "../android/host/user32.c"
#include "../android/host/system.c"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;
int void_verbose;
static uint32_t last_error;
void void_set_last_error(uint32_t error) { last_error = error; }
void void_log(const char *fmt, ...) { (void)fmt; }
uint32_t void_thread_id(void) { return 0x104; }
void void_input_pump(void) {}
void void_input_cursor(uint32_t *x, uint32_t *y) { *x = 0; *y = 0; }
void void_wait_broadcast(void) {}
uint32_t void_wait_extra_cond(int n, const uint32_t *handles, uint32_t timeout, int (*extra)(void *), void *arg)
{
    (void)n; (void)handles; (void)timeout; (void)extra; (void)arg;
    assert(0); return WAIT_FAILED_;
}
int void_vm_readable(uint32_t p, uint32_t n) { return p >= 0x1000 && p < 0x10000 && n <= 0x10000u - p; }
int void_vm_writable(uint32_t p, uint32_t n) { return void_vm_readable(p, n); }
int void_ddraw_dc_pixels(uint32_t dc, uint32_t *dst, int *w, int *h, int *pitch, int *bpp)
{
    if (dc != 0x70000) return 0;
    *dst = 0x8000; *w = 8; *h = 6; *pitch = 16; *bpp = 16;
    return 1;
}
int void_android_text(uint32_t dst, int w, int h, int pitch, int bpp, const uint16_t *text,
    int count, int size, int weight, int italic, int x, int y, uint32_t color, uint32_t bg,
    int opaque, int *extent_w, int *extent_h)
{
    (void)text; (void)bg; (void)opaque;
    assert(size == -20 && weight == 700 && italic == 1);
    *extent_w = count * 10; *extent_h = 20;
    if (dst) {
        assert(bpp == 16 && w == 4 && h == 3 && pitch == 16 && x == 1 && y == 0);
        MEM16(dst + (uint32_t)(y * pitch + x * 2)) = gdi_rgb565(color);
    }
    return 1;
}

static void clear_args(void) { g_esp = 0x1000; memset(GPTR(g_esp), 0, 128); }

int main(void)
{
    assert(mmap((void *)(uintptr_t)VOID_GUEST_BASE, 0x10000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)VOID_GUEST_BASE);
    clear_args(); ARG(0) = (uint32_t)-20; ARG(4) = 700; ARG(5) = 1;
    uint32_t font = CreateFontA_impl(); assert(font);
    clear_args(); ARG(0) = 0x70000; ARG(1) = font;
    uint32_t previous = SelectObject_impl(); assert(previous == 0x5000D);
    ARG(1) = 0xFF; assert(SetTextColor_impl() == 0);
    ARG(1) = 0xFF00; assert(SetBkColor_impl() == 0xFFFFFF);
    ARG(1) = 1; assert(SetBkMode_impl() == 2);
    MEM16(0x3000) = 'A'; MEM16(0x3002) = 'B';
    clear_args(); ARG(0) = 0x70000; ARG(1) = 0x3000; ARG(2) = 2; ARG(3) = 0x3100;
    assert(GetTextExtentPoint32W_impl() == 1 && MEM32(0x3100) == 20 && MEM32(0x3104) == 20);
    ARG(3) = 15; ARG(4) = 0x3120; ARG(5) = 0x3130; ARG(6) = 0x3100;
    assert(GetTextExtentExPointW_impl() == 1 && MEM32(0x3120) == 1 && MEM32(0x3130) == 10 && MEM32(0x3134) == 20);
    MEM32(0x3200) = 1; MEM32(0x3204) = 1; MEM32(0x3208) = 5; MEM32(0x320C) = 4;
    clear_args(); ARG(0) = 0x70000; ARG(1) = 2; ARG(2) = 1; ARG(3) = 6;
    ARG(4) = 0x3200; ARG(5) = 0x3000; ARG(6) = 2;
    assert(ExtTextOutW_impl() == 1);
    assert(MEM16(0x8000 + 16 + 2) == 0x07E0 && MEM16(0x8000 + 16 + 4) == 0xF800 && MEM16(0x8000) == 0);
    clear_args(); ARG(0) = font; assert(DeleteObject_impl() == 0);
    ARG(0) = 0x70000; ARG(1) = previous; assert(SelectObject_impl() == font);
    ARG(0) = font; assert(DeleteObject_impl() == 1);

    struct tm tm = { .tm_year = 100, .tm_mon = 0, .tm_mday = 2, .tm_hour = 3, .tm_min = 4, .tm_sec = 6 };
    uint64_t ft = (uint64_t)timegm(&tm) * 10000000ull + 116444736000000000ull;
    MEM32(0x3300) = (uint32_t)ft; MEM32(0x3304) = (uint32_t)(ft >> 32);
    clear_args(); ARG(0) = 0x3300; ARG(1) = 0x3310; ARG(2) = 0x3312;
    assert(FileTimeToDosDateTime_impl() == 1);
    assert(MEM16(0x3310) == ((20 << 9) | (1 << 5) | 2) && MEM16(0x3312) == ((3 << 11) | (4 << 5) | 3));
    MEM32(0x3300) = MEM32(0x3304) = 0;
    assert(FileTimeToDosDateTime_impl() == 0 && last_error == ERROR_INVALID_PARAMETER_);

    strcpy((char *)GPTR(0x4100), "%08lX %s %d %%"); strcpy((char *)GPTR(0x4200), "test");
    clear_args(); ARG(0) = 0x4000; ARG(1) = 0x4100; ARG(2) = 0xABCD; ARG(3) = 0x4200; ARG(4) = (uint32_t)-7;
    assert(wsprintfA_impl() == strlen("0000ABCD test -7 %"));
    assert(!strcmp((char *)GPTR(0x4000), "0000ABCD test -7 %"));

    clear_args(); ARG(2) = 10;
    uint32_t timer_id = SetTimer_impl(); assert(timer_id);
    user_timers[0].next = void_tick_ms() - 1; pump_host_input();
    queue_t *q = queue_for(0x104); assert(q && q->count == 1);
    user_timers[0].next = void_tick_ms() - 1; pump_host_input();
    assert(q->count == 1); /* WM_TIMER coalesces while the old message is pending. */
    qmsg_t message;
    assert(queue_take(q, 0, 0, 0, 1, &message) && message.msg == 0x113 && message.wparam == timer_id);
    user_timers[0].next = void_tick_ms() - 1; pump_host_input(); assert(q->count == 1);
    clear_args(); ARG(1) = timer_id; ARG(2) = 20;
    assert(SetTimer_impl() == timer_id && user_timers[0].interval == 20);
    assert(KillTimer_impl() == 1 && !user_timers[0].active);
    assert(queue_take(q, 0, 0, 0, 1, &message) && q->count == 0);

    clear_args(); ARG(1) = 0x400; ARG(2) = 3; ARG(3) = 4;
    assert(PostMessageW_impl() == 1);
    clear_args(); ARG(0) = 0x3500;
    GetMessageW_entry();
    assert(g_eax == 1 && g_esp == 0x1014 && MEM32(0x3504) == 0x400 && MEM32(0x3508) == 3 && MEM32(0x350C) == 4);
    clear_args(); ARG(0) = 23; PostQuitMessage_impl();
    clear_args(); ARG(0) = 0x3500;
    assert(GetMessageA_impl() == 0 && MEM32(0x3504) == WM_QUIT && MEM32(0x3508) == 23);
    clear_args(); assert(GetMessageW_impl() == 0xFFFFFFFFu && last_error == ERROR_INVALID_PARAMETER_);

    MEM32(0x3400) = 284; clear_args(); ARG(0) = 0x3400;
    assert(GetVersionExW_impl() == 1 && MEM32(0x3404) == 6 && MEM32(0x3408) == 2 && MEM8(0x3400 + 282) == 1);
    MEM32(0x3400) = 276; assert(GetVersionExW_impl() == 1 && MEM16(0x3414) == 0);
    MEM32(0x3400) = 148; assert(GetVersionExW_impl() == 0 && GetVersionExA_impl() == 1);
    puts("win32_selfcheck: GDI, DOS date, wsprintf, timer, Unicode PostMessage/GetMessage ABI, A/W version structs PASS");
    return 0;
}
