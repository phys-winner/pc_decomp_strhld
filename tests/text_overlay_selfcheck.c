/* Crisp GDI text: noting, verification, hiding, z-order and drawing with the
 * real display.c layer model (capture, HUD copy, layer_view) and the real
 * raster blend of gdi_android.c. The Android rasterizer is a stand-in that
 * records its request and fills the scaled raster box; the GL part draws the
 * queued quads into an EGL pbuffer and reads them back.
 * Build (NDK): clang --target=aarch64-linux-android30 -O2 -std=gnu11 -Iandroid/runtime -Iandroid/host
 *   [-DSTRONGHOLD_CLASSIC] tests/text_overlay_selfcheck.c -lm -lEGL -lGLESv3 -Wl,-z,max-page-size=16384 */
#define _GNU_SOURCE
#include "../android/host/display.c"
#include "../android/host/text_overlay.h"
#define void_android_text_raster unused_android_text_raster
#include "../android/host/gdi_android.c"
#undef void_android_text_raster
#include "../android/host/text_overlay.c"
#include <EGL/egl.h>
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define GROUPS 0x11000000u
#define HUD 0x19000000u
#define HUD_PITCH 2560u
#define W 1280
#define H 720

void void_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); putchar('\n'); va_end(a); }
void stronghold_feedback(int bits) { (void)bits; }
int void_verbose = 1;   /* the overlay's statistics line too */
void stronghold_perf_frame_begin(void) {}
uint64_t void_monotonic_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void void_input_set_screen_size(int w, int h) { (void)w; (void)h; }
static uint32_t next_group = GROUPS;
uint32_t void_vm_alloc(uint32_t p, uint32_t n, uint32_t t, uint32_t prot)
{ (void)p; (void)t; (void)prot; uint32_t a = next_group; next_group += (n + 0xFFFFu) & ~0xFFFFu; return a; }
uint32_t void_ddraw_redirect_pixels(uint32_t s, uint32_t p) { (void)s; return p; }
int void_vm_writable(uint32_t va, uint32_t n)
{
    uint64_t end = (uint64_t)va + n;
    return (va >= (GAME_SCREEN & ~0xFFFu) && end <= (GAME_SCREEN & ~0xFFFu) + 0x2000u) ||
           (va >= GROUPS && end <= GROUPS + 0x7800000u) || (va >= HUD && end <= HUD + 0x200000u);
}
int void_vm_readable(uint32_t va, uint32_t n) { return void_vm_writable(va, n); }

/* Stand-in rasterizer: the scaled raster box is fully covered, its first
 * column half; the request is kept for the assertions. */
static struct { int calls, count, size, weight, italic, w, h; float scale, dx, dy; uint16_t text[64]; } raster;
int void_android_text_raster(const uint16_t *text, int count, int size, int weight, int italic,
    float scale, float dx, float dy, int width, int height, uint8_t *out)
{
    raster.calls++; raster.count = count; raster.size = size; raster.weight = weight; raster.italic = italic;
    raster.scale = scale; raster.dx = dx; raster.dy = dy; raster.w = width; raster.h = height;
    memcpy(raster.text, text, (size_t)(count < 64 ? count : 64) * 2);
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++)
        out[(size_t)y * width + x] = x == (int)ceilf(dx) ? 128 : 255;
    return 1;
}

static uint16_t *pixel(int g, int x, int y)
{ return GPTR(ui[g].guest + MAP_PITCH * (uint32_t)(origin_y + y) + 2u * (uint32_t)(origin_x + x)); }
static uint16_t *hud(int x, int y) { return GPTR(HUD + HUD_PITCH * (uint32_t)y + 2u * (uint32_t)x); }

/* An opaque UI element in the current group: its stores are what the
 * original raster families report. */
static void fill(int g, int x, int y, int w, int h, uint16_t color)
{
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) *pixel(g, x + c, y + r) = color;
        if (stronghold_display_capture_active) stronghold_display_written(GVA(pixel(g, x, y + r)), (unsigned)w * 2);
    }
}

static void fill_hud(int x, int y, int w, int h, uint16_t color)
{ for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) *hud(x + c, y + r) = color; }

/* A 24x12 run: blank top rows like a font's ascent, stems with soft edges. */
static uint8_t mask[12 * 24];
static void make_mask(void)
{
    memset(mask, 0, sizeof mask);
    static const uint8_t stems[4] = { 255, 160, 48, 96 };
    for (int y = 3; y < 10; y++) for (int x = 2; x < 22; x++) mask[y * 24 + x] = stems[x % 4];
}

static const uint16_t cyrillic[] = { 0x0422, 0x0435, 0x043A, 0x0441, 0x0442 }; /* "Текст" */
static void run_at(uint32_t dst, int w, int h, int pitch, int x, int y, uint32_t color, uint32_t bg, int opaque)
{
    void_android_blend(dst, w, h, pitch, 16, mask, 24, 12, 24, cyrillic, 5, -12, 700, 1, x, y, color, bg, opaque);
}
/* The back buffer as a DC sees it: the whole 4056-wide surface, UI drawn at the view origin. */
static void run(int g, int x, int y, uint32_t color, uint32_t bg, int opaque)
{ run_at(ui[g].guest, 4056, 2076, MAP_PITCH, origin_x + x, origin_y + y, color, bg, opaque); }
static void run_hud(int x, int y, uint32_t color)
{ run_at(HUD, W, H, HUD_PITCH, x, y, color, 0, 0); }

static void frame_start(void)
{
    stronghold_display_frame_begin();
    origin_x = 8; origin_y = 4; color_key = 0xF81F;
    hud_pixels = HUD; hud_pitch = HUD_PITCH;
    MEM32(GAME_SCREEN + 0xd4) = HUD; MEM32(GAME_SCREEN + 0x40) = HUD_PITCH; MEM32(GAME_SCREEN + 0xc0) = 0;
    original_pixels = 0; original_surface = 1;
    assert(select_group(0));
    stronghold_display_capture_active = ui_open = 1;
}
static void enter(int g) { previous_group = current_group; assert(select_group(g)); }
static void frame_end(void) { stronghold_display_ui_end(); assert(frame_ready == 1); }
/* The old presenter's order: hide, then compose (which marks the frame used). */
static void publish(void) { stronghold_text_overlay_publish(); frame_ready = 2; }

static int same_region(int g, int x, int y, int w, int h, uint16_t color)
{
    for (int r = 0; r < h; r++) for (int c = 0; c < w; c++)
        if (ui[g].pixels[(size_t)(y + r) * W + x + c] != color) return 0;
    return 1;
}
static int raster_in(int g, int x, int y, int w, int h, uint16_t background)
{ return !same_region(g, x, y, w, h, background); }

static void basic_hide(void)
{
    frame_start(); enter(2);
    fill(2, 100, 100, 200, 80, 0x4208);
    run(2, 120, 110, 0x00FFFFFF, 0, 0);
    assert(note_count == 1 && notes[0]->kind == 2 && notes[0]->x == 122 && notes[0]->y == 113);
    assert(notes[0]->w == 20 && notes[0]->h == 7 && notes[0]->target == 2 && !notes[0]->coverage);
    enter(0); frame_end();
    assert(raster_in(2, 122, 113, 20, 7, 0x4208));
    raster.calls = 0;
    publish();
    /* The presented layer is text-free, the guest's surface keeps its raster text. */
    assert(same_region(2, 100, 100, 200, 80, 0x4208));
    assert(*pixel(2, 124, 113) == 0xFFFF); /* a fully covered stem */
    assert(queues[2].count == 1 && queues[0].count == 0 && note_count == 0);
    const quad_t *q = &queues[2].quads[0];
    /* The raster box 120,110 24x12 at 2x, plus MARGIN original pixels around it. */
    assert(q->x0 == 240 - 2 * MARGIN && q->y0 == 220 - 2 * MARGIN && q->x1 == 288 + 2 * MARGIN && q->y1 == 244 + 2 * MARGIN);
    assert(raster.calls == 1 && raster.scale == 2.f && raster.dx == 2 * MARGIN && raster.dy == 2 * MARGIN &&
           raster.w == 48 + 4 * MARGIN && raster.h == 24 + 4 * MARGIN);
    assert(raster.count == 5 && raster.text[0] == 0x0422 && raster.size == -12 && raster.weight == 700 && raster.italic == 1);
    assert(q->rgba == 0xFFFFFFFFu);
    /* A second publish of the same frame finds nothing new to hide. */
    publish();
    assert(queues[2].count == 1);
    puts("basic: noted in a dialog, text-free layer, guest raster kept, quad at 2x, Cyrillic run request PASS");
}

static void overdraw_and_order(void)
{
    /* Something painted over the run after it: the raster stays. */
    frame_start(); enter(2);
    fill(2, 100, 100, 200, 80, 0x4208);
    run(2, 120, 110, 0x00FFFFFF, 0, 0);
    fill(2, 125, 115, 3, 3, 0x07E0);
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 0 && raster_in(2, 122, 113, 20, 7, 0x4208));
    /* Two overlapping runs: both hidden, their overlap text-free, older drawn first. */
    frame_start(); enter(2);
    fill(2, 100, 100, 200, 80, 0x4208);
    run(2, 120, 110, 0x000000FF, 0, 0);
    run(2, 130, 112, 0x0000FF00, 0, 0);
    assert(note_count == 2);
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 2 && same_region(2, 100, 100, 200, 80, 0x4208));
    assert(queues[2].quads[0].rgba == 0xFF0000FFu && queues[2].quads[1].rgba == 0xFF00FF00u);
    /* The newer run is changed outside the older one's box: hiding the older
     * would erase the newer's raster text, so both keep theirs. */
    frame_start(); enter(2);
    fill(2, 100, 100, 200, 80, 0x4208);
    run(2, 120, 110, 0x000000FF, 0, 0);
    run(2, 130, 112, 0x0000FF00, 0, 0);
    fill(2, 150, 117, 2, 2, 0x07E0);
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 0 && raster_in(2, 122, 113, 8, 7, 0x4208));
    /* An opaque run over an older one: the older loses glyphs and is dropped,
     * the newer is hidden down to its own background box. */
    frame_start(); enter(2);
    fill(2, 100, 100, 200, 80, 0x4208);
    run(2, 120, 110, 0x000000FF, 0, 0);
    run(2, 126, 111, 0x00FFFFFF, 0x00102030, 1);
    assert(note_count == 1);
    enter(0); frame_end();
    publish();
    uint16_t box = (uint16_t)((0x30 >> 3) << 11 | (0x20 >> 2) << 5 | 0x10 >> 3);
    assert(queues[2].count == 1 && same_region(2, 126, 111, 24, 12, box));
    assert(raster_in(2, 122, 113, 4, 7, 0x4208)); /* the older run's visible part keeps its raster text */
    puts("z-order: overdraw keeps raster, overlaps hidden together, newer changed blocks older, opaque box PASS");
}

static void occluded(void)
{
    /* Old presenter (one composed texture, all text after it): a lower group's
     * run under a higher group's box keeps its raster copy; the others don't. */
    frame_start();
    fill(0, 100, 100, 200, 24, 0x2104); run(0, 120, 106, 0x00FFFFFF, 0, 0);
    fill(0, 400, 20, 200, 40, 0x2104); run(0, 420, 30, 0x00FFFFFF, 0, 0);
    enter(2); fill(2, 90, 90, 300, 100, 0x4208); run(2, 200, 150, 0x00FFFFFF, 0, 0);
    enter(0); frame_end();
    unsigned blocked = stats.blocked;
    publish();
    assert(queues[0].count == 1 && queues[2].count == 1 && stats.blocked == blocked + 1);
    assert(raster_in(0, 122, 109, 20, 7, 0x2104) && same_region(0, 400, 20, 200, 40, 0x2104));
    assert(!occluder[1] && !occluder[2]);
    puts("old presenter: HUD run under a dialog box keeps raster, others hidden PASS");
}

static int coverage_is(int g, int x, int y, int w, int h, uint8_t value)
{
    for (int r = 0; r < h; r++) for (int c = 0; c < w; c++)
        if (ui[g].coverage[(size_t)(y + r) * W + x + c] != value) return 0;
    return 1;
}

static void coverage_rules(void)
{
    /* Over the world (nothing below in the layer) the run raised coverage:
     * hidden with the coverage it found, so the world shows under crisp text. */
    frame_start(); enter(2);
    run(2, 400, 300, 0x00FFFFFF, 0, 0);
    assert(note_count == 1 && notes[0]->coverage && notes[0]->target == 2);
    enter(0); frame_end();
    assert(!coverage_is(2, 402, 303, 20, 7, 0));
    publish();
    assert(queues[2].count == 1 && coverage_is(2, 402, 303, 20, 7, 0) && same_region(2, 402, 303, 20, 7, 0xF81F));
    /* A blended (half transparent) dialog background keeps its own coverage. */
    frame_start(); enter(2);
    for (int r = 0; r < 40; r++) {
        for (int c = 0; c < 100; c++) *pixel(2, 380 + c, 290 + r) = 0x39E7;
        stronghold_display_blended(GVA(pixel(2, 380, 290 + r)), 200, 128);
    }
    run(2, 400, 300, 0x00FFFFFF, 0, 0);
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 1 && coverage_is(2, 380, 290, 100, 40, 127) && same_region(2, 380, 290, 100, 40, 0x39E7));
    /* A presenter that cannot edit coverage keeps such runs as raster. */
    frame_start(); enter(2);
    run(2, 400, 300, 0x00FFFFFF, 0, 0);
    enter(0); frame_end();
    uint16_t *layers[SH_UI_LAYERS]; uint8_t *coverage[SH_UI_LAYERS], *none[SH_UI_LAYERS] = { 0 }; int bounds[SH_UI_LAYERS][4], w, h;
    assert(stronghold_display_text_layers(layers, coverage, bounds, &w, &h));
    unsigned refused = stats.coverage;
    int hidden[SH_UI_LAYERS][4];
    assert(stronghold_text_overlay_hide_layers(layers, none, bounds, w, w, h, hidden) == 0 && stats.coverage == refused + 1);
    assert(hidden[2][0] == 0 && hidden[2][2] == 0);
    frame_ready = 2;
    assert(queues[2].count == 0 && raster_in(2, 402, 303, 20, 7, 0xF81F));
    /* Opaque: the whole box is written with or without glyphs. */
    frame_start(); enter(2);
    run(2, 400, 300, 0x00FFFFFF, 0x00000000, 1);
    assert(note_count == 1 && !notes[0]->coverage);
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 1 && same_region(2, 400, 300, 24, 12, 0) && coverage_is(2, 400, 300, 24, 12, 255));
    /* A presenter that uploads changed rows only learns what was edited. */
    frame_start(); enter(2);
    fill(2, 100, 100, 200, 80, 0x4208);
    run(2, 120, 110, 0x00FFFFFF, 0, 0); run(2, 130, 130, 0x00FFFFFF, 0, 0);
    enter(0); frame_end();
    assert(stronghold_display_text_layers(layers, coverage, bounds, &w, &h));
    assert(stronghold_text_overlay_hide_layers(layers, coverage, bounds, w, w, h, hidden) == 2);
    frame_ready = 2;
    assert(hidden[2][0] == 122 && hidden[2][1] == 113 && hidden[2][2] == 152 && hidden[2][3] == 140 && !hidden[0][2]);
    puts("coverage: over the world and over a blended dialog restored, refused without coverage, opaque box PASS");
}

static void partial_rows(void)
{
    /* A list row's background painted after the row above it covers that
     * row's lowest ink: the rest is hidden and the crisp run is cut there. */
    frame_start(); enter(2);
    fill(2, 100, 100, 300, 20, 0x4208);
    run(2, 120, 108, 0x00FFFFFF, 0, 0);            /* ink rows 111..117 */
    fill(2, 100, 117, 300, 20, 0x2945);            /* next row from 117 */
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 1 && same_region(2, 100, 100, 300, 17, 0x4208) && same_region(2, 100, 117, 300, 20, 0x2945));
    float vx, vy, s;
    assert(stronghold_display_layer_view(2, 120, 108, &vx, &vy, &s));
    assert(queues[2].quads[0].y1 == (int)lrintf(vy + (117 - 108) * s) && queues[2].quads[0].y0 == (int)lrintf(vy - MARGIN * s));
    /* Changed in the middle: no band, the raster stays. */
    frame_start(); enter(2);
    fill(2, 100, 100, 300, 20, 0x4208);
    run(2, 120, 108, 0x00FFFFFF, 0, 0);
    fill(2, 100, 114, 300, 1, 0x2945);
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 0 && raster_in(2, 122, 111, 20, 3, 0x4208));
    puts("rows: lowest ink row painted over is cut, a change in the middle keeps raster PASS");
}

static void clipping_and_scale(void)
{
    /* ETO_CLIPPED: the drawable area starts inside the run. */
    frame_start(); enter(2);
    fill(2, 100, 100, 200, 80, 0x4208);
    uint32_t dst = GVA(pixel(2, 128, 108));
    run_at(dst, 10, 30, MAP_PITCH, 120 - 128, 110 - 108, 0x00FFFFFF, 0, 0);
    assert(note_count == 1 && notes[0]->x == 128 && notes[0]->w == 10);
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 1);
    const quad_t *q = &queues[2].quads[0];
    /* Cut where the clip cut glyphs (x), a pixel beyond the uncut box (y). */
    assert(q->x0 == 256 && q->x1 == 276 && q->y0 == 220 - 2 * MARGIN && q->y1 == 244 + 2 * MARGIN &&
           q->u - (q->x0 - (240 - 2 * MARGIN)) >= 0);
    /* HUD scale 150%: dialogs scale about the centre, group 0's top bar about 0,0. */
    frame_start(); ui_scale = 1.5f;
    fill(0, 0, 0, 300, 40, 0x2104);
    run(0, 10, 10, 0x00FFFFFF, 0, 0);
    enter(2); fill(2, 560, 300, 200, 80, 0x4208); run(2, 600, 320, 0x00FFFFFF, 0, 0);
    enter(0); frame_end(); ui_scale = 1;
    publish();
    assert(queues[0].count == 1 && queues[2].count == 1);
    assert(queues[0].quads[0].x0 == 30 - 3 * MARGIN && queues[0].quads[0].y0 == 30 - 3 * MARGIN &&
           queues[0].quads[0].x1 == 30 + 3 * (24 + MARGIN));
    float vx, vy, s;
    assert(stronghold_display_layer_view(2, 600, 320, &vx, &vy, &s) && s == 3.f);
    assert(queues[2].quads[0].x0 == (int)vx - 3 * MARGIN && queues[2].quads[0].y0 == (int)vy - 3 * MARGIN && raster.scale == 3.f);
    puts("clip/scale: ETO_CLIPPED quad, 150% HUD top bar and centred dialog transforms PASS");
}

static void hud_surface(void)
{
    /* Text in the persistent HUD surface, drawn outside a pass, reaches group 0
     * through the copy of the bottom rows and stays while that surface holds it. */
    fill_hud(0, 592, W, 128, 0x2104);
    stronghold_display_capture_active = ui_open = 0;
    run_hud(300, 650, 0x0000FFFF);
    assert(note_count == 1 && notes[0]->kind == SH_UI_LAYERS && notes[0]->target == -1);
    for (int frame = 0; frame < 3; frame++) {
        frame_start(); frame_end();
        assert(raster_in(0, 302, 653, 20, 7, 0x2104));
        publish();
        assert(queues[0].count == 1 && same_region(0, 0, 592, W, 128, 0x2104) && note_count == 1);
    }
    assert(*hud(302, 653) != 0x2104);
    /* The HUD redraws that area without the text: the note is gone. */
    fill_hud(290, 640, 60, 30, 0x2104);
    frame_start(); frame_end();
    publish();
    assert(queues[0].count == 0 && note_count == 0);
    /* The pass's own drawing under the HUD rows is covered by that copy. */
    frame_start(); fill(0, 300, 600, 40, 20, 0x4208); run(0, 302, 602, 0x00FFFFFF, 0, 0); frame_end();
    publish();
    assert(queues[0].count == 0 && note_count == 0);
    /* Drawn into the HUD surface during a pass: the stores are mirrored into
     * the current group; over an opaque mirrored background it is hidden there. */
    fill_hud(0, 592, W, 128, 0x2104);
    frame_start(); enter(2);
    for (int r = 0; r < 40; r++) { for (int c = 0; c < 100; c++) *hud(500 + c, 200 + r) = 0x39E7;
        stronghold_display_written(HUD + HUD_PITCH * (uint32_t)(200 + r) + 1000u, 200); }
    run_hud(510, 210, 0x00FFFFFF);
    assert(note_count == 1 && notes[0]->target == 2 && !notes[0]->coverage); /* mirrored over an opaque background */
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 1 && same_region(2, 500, 200, 100, 40, 0x39E7));
    /* Next frame the mirror is gone (the pass did not repeat it). */
    frame_start(); frame_end();
    publish();
    assert(queues[2].count == 0 && queues[0].count == 0);
    for (int r = 200; r < 240; r++) for (int c = 500; c < 600; c++) *hud(c, r) = 0;
    puts("HUD surface: bottom-row copy across frames, redraw drops it, pass drawing under the copy, mirrored dialog PASS");
}

static void resource_notice(void)
{
    /* Original overlay4: left+bottom flags, at x10, above the bottom HUD.
     * The wrapper selects its own raster grid before the real GDI stores. */
    const uint32_t item=HUD+0x1f0000u;
    for(int percent=75;percent<=200;percent+=5){
        stronghold_display_set_ui_scale(percent/100.f);
        frame_start();
        MEM32(item+0x18)=0x004AFFA0u;MEM32(item+0x1c)=6;
        stronghold_display_resource_overlay(item,1);assert(current_group==3);
        run(3,10,520,0x00FFFFFF,0,0);
        assert(note_count==1&&notes[0]->kind==3&&notes[0]->target==3);
        stronghold_display_resource_overlay(item,0);assert(current_group==0);
        frame_end();publish();assert(queues[3].count==1);
        float x,y,s;assert(stronghold_display_layer_view(3,10,520,&x,&y,&s));
        float actual=published_ui_scale;
        assert(fabsf(x-20*actual)<.01f&&fabsf(y-2*(H+(520-H)*actual))<.01f);
        assert(fabsf(queues[3].quads[0].x0-(x-MARGIN*s))<=1&&queues[3].quads[0].x0>=0);
    }
    stronghold_display_set_ui_scale(1);
    frame_start();stronghold_display_resource_overlay(item,1);
    run(3,10,520,0x00FFFFFF,0,0);stronghold_display_resource_overlay(item,0);
    enter(2);fill(2,8,518,40,20,0x07e0);enter(0);frame_end();
    unsigned blocked=stats.blocked;publish();assert(queues[3].count==0&&stats.blocked==blocked+1);
    frame_start();frame_end();publish();assert(queues[3].count==0);
    puts("resource notice: original flags choose group3; Cyrillic GDI text keeps left+bottom anchor75..200%, dialog occlusion, expiry clears PASS");
}

static void menu_frame(void)
{
    /* Menus present the HUD surface as the whole frame (group -1). */
    frame_start(); frame_end(); publish();
    stronghold_display_frame_begin(); published_active = 0;
    stronghold_display_capture_active = ui_open = 0;
    fill_hud(0, 0, W, H, 0x18C3);
    run_hud(50, 40, 0x00FFFFFF);
    run_hud(60, 41, 0x0000FF00);
    static uint16_t frame[W * H], copy[W * H];
    for (int y = 0; y < H; y++) memcpy(frame + (size_t)y * W, hud(0, y), W * 2);
    memcpy(copy, frame, sizeof frame);
    assert(stronghold_text_overlay_hide_frame(frame, W, W, H) == 2 && queues[SH_UI_LAYERS].count == 2);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) assert(frame[(size_t)y * W + x] == 0x18C3);
    assert(queues[SH_UI_LAYERS].quads[0].x0 == 100 - 2 * MARGIN && queues[SH_UI_LAYERS].quads[0].y0 == 80 - 2 * MARGIN); /* 50,40 at 2x */
    stronghold_text_overlay_restore_frame();
    assert(!memcmp(frame, copy, sizeof frame));
    /* The frame shows something else there (a newer present): nothing hidden, note kept. */
    for (int x = 40; x < 100; x++) frame[45 * W + x] = 0x07E0;
    assert(stronghold_text_overlay_hide_frame(frame, W, W, H) == 0 && queues[SH_UI_LAYERS].count == 0 && note_count == 2);
    /* The surface itself changed: both notes go. */
    fill_hud(40, 40, 60, 20, 0x18C3);
    for (int y = 0; y < H; y++) memcpy(frame + (size_t)y * W, hud(0, y), W * 2);
    assert(stronghold_text_overlay_hide_frame(frame, W, W, H) == 0 && note_count == 0);
    puts("menu frame: hidden in place, restored byte-exact, newer frame content and surface change PASS");
}

static void disabled(void)
{
    stronghold_display_set_filters(3, 3, .25f, 0, 0);
    frame_start(); enter(2);
    fill(2, 100, 100, 200, 80, 0x4208);
    run(2, 120, 110, 0x00FFFFFF, 0, 0);
    assert(note_count == 0);
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 0 && raster_in(2, 122, 113, 20, 7, 0x4208));
    stronghold_display_set_filters(3, 3, .25f, 0, 1);
    puts("setting off: nothing noted, raster text exactly as before PASS");
}

static void atlas_cycle(void)
{
    /* Distinct runs every frame: shelves are recycled, glyphs reused while alive. */
    unsigned failed = 0;
    for (int frame = 0; frame < 400; frame++) {
        frame_start(); enter(2);
        fill(2, 0, 100, W, 400, 0x4208);
        for (int i = 0; i < 12; i++) {
            uint16_t text[3] = { (uint16_t)('A' + i), (uint16_t)(0x0410 + frame % 32), (uint16_t)('0' + frame / 32) };
            void_android_blend(ui[2].guest, 4056, 2076, MAP_PITCH, 16, mask, 24, 12, 24, text, 3, -12 - frame % 7, 400, 0,
                               origin_x + 40 + i * 90, origin_y + 120 + (frame % 5) * 30, 0x00FFFFFF, 0, 0);
        }
        enter(0); frame_end();
        unsigned placed = stats.placed;
        publish();
        failed += stats.placed - placed;
        assert(queues[2].count + (int)(stats.placed - placed) == 12);
    }
    assert(!failed);
    int alive = 0;
    for (int i = 0; i < glyph_top; i++) alive += glyphs[i].used;
    assert(alive <= MAX_GLYPHS && shelf_count > 0);
    puts("atlas: 4800 distinct runs over 400 frames, shelf eviction without a failed placement PASS");
}

static double frame_ms(int crisp)
{
    /* 120 runs in one dialog, the pass's end and the publish; best of 20.
     * The stand-in rasterizer is cached after the first round. */
    double best = 1e9;
    stronghold_display_set_filters(3, 3, .25f, 0, crisp);
    for (int round = 0; round < 20; round++) {
        frame_start(); enter(2);
        fill(2, 0, 100, W, 500, 0x4208);
        uint64_t t0 = void_monotonic_ns();
        for (int i = 0; i < 120; i++) run(2, 20 + (i % 12) * 100, 110 + (i / 12) * 40, 0x00FFFFFF, 0, 0);
        enter(0); frame_end();
        publish();
        double ms = (double)(void_monotonic_ns() - t0) / 1e6;
        if (ms < best) best = ms;
        assert(queues[2].count == (crisp ? 120 : 0));
    }
    stronghold_display_set_filters(3, 3, .25f, 0, 1);
    return best;
}

/* ------------------------------------------------------------------ GL */
static void gl_draw(void)
{
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(eglInitialize(d, NULL, NULL));
    const EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig config; EGLint n = 0;
    assert(eglChooseConfig(d, attrs, &config, 1, &n) && n);
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE }, pa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLContext c = eglCreateContext(d, config, EGL_NO_CONTEXT, ca);
    EGLSurface s = eglCreatePbufferSurface(d, config, pa);
    assert(c != EGL_NO_CONTEXT && s != EGL_NO_SURFACE && eglMakeCurrent(d, s, s, c));
    /* One dialog run, as basic_hide, in a 1:1 window: the box 120,110-144,122 plus MARGIN. */
    stronghold_display_surface_size(W, H);
    frame_start(); enter(2);
    fill(2, 100, 100, 200, 80, 0x4208);
    run(2, 120, 110, 0x0000FF00, 0, 0);
    enter(0); frame_end();
    publish();
    assert(queues[2].count == 1);
    const quad_t q = queues[2].quads[0];
    assert(q.x0 == 120 - MARGIN && q.y0 == 110 - MARGIN && q.x1 == 144 + MARGIN && q.y1 == 122 + MARGIN);
    glViewport(0, 0, W, H); glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
    stronghold_text_overlay_draw(2, W, H);
    assert(glGetError() == GL_NO_ERROR);
    int pending_left = 0;
    for (int i = 0; i < glyph_top; i++) pending_left += glyphs[i].used && glyphs[i].pending;
    assert(!pending_left);
    static uint8_t rgba[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* Window rows are top-down, GL rows bottom-up. */
#define AT(x, y) (rgba + ((size_t)(H - 1 - (y)) * W + (x)) * 4)
    for (int y = q.y0 - 2; y < q.y1 + 2; y++) for (int x = q.x0 - 2; x < q.x1 + 2; x++) {
        const uint8_t *p = AT(x, y);
        int inside = x >= q.x0 && x < q.x1 && y >= q.y0 && y < q.y1;
        if (!inside) assert(p[0] == 0 && p[1] == 0 && p[2] == 255);
        else if (x == q.x0 + MARGIN) assert(p[1] > 120 && p[1] < 136 && p[2] > 119 && p[2] < 136); /* half coverage */
        else assert(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255);
    }
    /* Drawn again (a re-present of the same frame): identical, nothing uploaded. */
    glClear(GL_COLOR_BUFFER_BIT);
    stronghold_text_overlay_draw(2, W, H);
    static uint8_t again[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, again);
    assert(!memcmp(rgba, again, sizeof rgba));
    /* Other groups have nothing queued and draw nothing. */
    glClear(GL_COLOR_BUFFER_BIT);
    stronghold_text_overlay_draw(0, W, H); stronghold_text_overlay_draw(-1, W, H);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, again);
    for (size_t i = 0; i < (size_t)W * H; i++) assert(again[i * 4 + 2] == 255 && again[i * 4 + 1] == 0);
    GLboolean blend = 1; glGetBooleanv(GL_BLEND, &blend); assert(!blend);
    GLint bound = 1; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound); assert(!bound);
    stronghold_text_overlay_reset_gl();
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(d, s); eglDestroyContext(d, c); eglTerminate(d);
    stronghold_display_surface_size(2 * W, 2 * H);
    puts("GL: atlas upload once, exact quad edges, coverage blend, re-draw identical, state restored PASS");
}

int main(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    assert(mmap((void *)(VOID_GUEST_BASE + (GAME_SCREEN & ~(uint32_t)(page - 1))), 2 * page, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    assert(mmap((void *)(VOID_GUEST_BASE + GROUPS), 0x7800000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    assert(mmap((void *)(VOID_GUEST_BASE + HUD), 0x200000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    stronghold_display_configure(W, H);
    assert(allocate_frames());
    stronghold_display_surface_size(2 * W, 2 * H);
    make_mask();
    int x, y;
    MEM32(GAME_SCREEN + 0xd4) = HUD; MEM32(GAME_SCREEN + 0x40) = HUD_PITCH;
    assert(stronghold_display_locate(HUD + HUD_PITCH * 7 + 22, &x, &y) == SH_UI_LAYERS && x == 11 && y == 7);
    assert(stronghold_display_locate(HUD + HUD_PITCH * H, &x, &y) == -1);
    /* A surface of another pitch inside a layer's memory is not that layer's grid. */
    frame_start(); enter(2);
    unsigned unlocated = stats.unlocated;
    run_at(GVA(pixel(2, 300, 300)), 64, 32, 256, 0, 0, 0x00FFFFFF, 0, 0);
    assert(note_count == 0 && stats.unlocated == unlocated + 1);
    enter(0); frame_end(); publish();
    basic_hide();
    overdraw_and_order();
    occluded();
    coverage_rules();
    partial_rows();
    clipping_and_scale();
    resource_notice();
    hud_surface();
    menu_frame();
    disabled();
    atlas_cycle();
    double off = frame_ms(0), on = frame_ms(1);
    printf("performance: 120 runs + pass end + publish: %.3f ms with crisp text, %.3f ms raster only (%.1f us per run)\n",
           on, off, (on - off) * 1000 / 120);
    gl_draw();
    puts("crisp text overlay selfcheck: PASS");
    return 0;
}
