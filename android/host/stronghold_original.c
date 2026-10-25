/* The original Stronghold HD (Stronghold.exe 8b581f48...): the engine shares
 * Crusader's lineage but not its addresses, so touch.c, display.c and
 * game_ui.c are not linked into this edition. This file supplies their public
 * APIs: a frame as wide as the screen (a table mode of the requested height
 * with its width replaced), the match's interface in layers of its own so the
 * GPU presenter scales it apart from the world, the main-menu state for the
 * game switch, and touch as a direct mouse: the semantic gestures of
 * SemanticTouchControls become the original mouse/keyboard input.
 * Addresses below belong to that SHA-pinned image (tools/original_hooks.py). */
#include "host.h"
#include "edition.h"
#include "input.h"
#include "display.h"
#include "touch.h"
#include "game_ui.h"
#include "game_text.h"
#include "original_construction.h"
#include "original_radar.h"
#include "original_options.h"
#include "text_overlay.h"
#include "feedback.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define STATE 0x01503D38u        /* screen/state id: 12, 14, 16 are a running map (45C610), 41 the main menu */
#define DIALOG 0x01503CE4u       /* open dialog, -1 for none */
#define CAMERA 0x016E9358u       /* view: +74 x, +78 y in world pixels, +84 tile columns, +8C zoomed out */
#define CAMERA_CLAMP 0x004B33B0u /* the original scroll code calls it after every move (42D50A) */
#define HUD_HEIGHT 140           /* bottom panel art with its top border; the view ends 128 px up (4B5070) */
#define SCREEN 0x00B182C8u       /* +18/+1C mode size, +38/+3C screen size, +40 UI pitch, +5C mode index */
#define SCREEN_WIDTH 0x00B18300u
#define SCREEN_HEIGHT 0x00B18304u
#define UI_PITCH 0x00B18308u
#define PRESENT_MODE 0x00B18398u /* +D0 in a match: 1 with the bottom panel, 2 without (45EC40) */
#define UI_PIXELS 0x00B1839Cu    /* +D4: the UI surface, menus and the bottom panel */
#define WORLD_PIXELS 0x00B183A0u /* +D8: the 4056x2076 world raster */
#define WORLD_SURFACE 0x00B183C8u /* +100: its DirectDraw surface (GDI text, Bink) */
#define RASTER_ORIGIN 0x016E93D4u /* camera +7C/+80: the view's offset inside the world raster */
#define UI_KEY 0x00A3A538u       /* the engine's transparent colour */
#define BOX_ORIGIN 0x00ABB980u   /* the centered 800x600 HUD box: x, then y */
#define RASTER_PITCH 8112u
#define RASTER_BYTES (RASTER_PITCH * 2076u)
#define SPEED_NOTICE 0x00A3BBFCu /* overlay 24 (552830), at 770,9 in the original 800x600 box */
#define HUD_BAND 128             /* panel rows the present copies from the UI surface */

uint32_t void_ddraw_redirect_pixels(uint32_t surface, uint32_t pixels);
static int notice_margin(void);

/* ------------------------------------------------------------ display mode */
/* The engine's own mode table (45E4A0); index 0 is "not configured yet". */
static const uint16_t modes[16][2] = {
    {0, 0}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1366, 768}, {1440, 900}, {1600, 900},
    {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600}, {1360, 768}, {1024, 600}};
static int requested_width, requested_height, chosen_mode, custom_width;

void stronghold_display_configure(int width, int height)
{
    requested_width = width > 0 ? width : 0;
    requested_height = height > 0 ? height : 0;
}

/* A wide frame keeps a table mode of its height: view rows, the HUD's place
 * and the overlays follow the mode index, only the width is replaced. 1, 2
 * and 5 have special cases of their own, so 600 and 768 use 15 and 14. */
static int height_mode(int height)
{
    static const int16_t table[][2] = {{600, 15}, {720, 3}, {768, 14}, {900, 6}, {1024, 4}, {1050, 9},
                                       {1080, 10}, {1200, 11}, {1440, 12}, {1600, 13}};
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i) if (table[i][0] == height) return table[i][1];
    return 0;
}

/* An exact original mode; else a custom width (800..2560, the widest stock
 * HUD edges) at a table height; else the largest 16:9 mode within the height. */
static int mode_for(int width, int height)
{
    custom_width = 0;
    for (int i = 1; i < 16; ++i) if (modes[i][0] == width && modes[i][1] == height) return i;
    int mode = height_mode(height);
    if (mode && width >= 800 && width <= 2560) {
        if ((width & ~7) != modes[mode][0]) custom_width = width & ~7;
        return mode;
    }
    static const int wide[] = {3, 5, 7, 10, 12};
    int best = 3;
    for (size_t i = 0; i < sizeof wide / sizeof wide[0]; ++i) if (modes[wide[i]][1] <= height) best = wide[i];
    return best;
}

/* tools/original_hooks.py: WinMain (532AAD) passes the saved mode index from
 * stronghold.cfg to the engine's mode switch (461540). */
uint32_t stronghold_original_mode(uint32_t saved)
{
    int width = requested_width, height = requested_height;
    const char *forced = getenv("STRONGHOLD_MODE");   /* shell bring-up: WxH */
    if (forced && sscanf(forced, "%dx%d", &width, &height) != 2) width = height = 0;
    custom_width = 0;
    chosen_mode = width && height ? mode_for(width, height) : saved >= 1 && saved <= 15 ? (int)saved : 3;
    int frame_w = custom_width ? custom_width : modes[chosen_mode][0];
    /* The mode switch reads the desktop size (458C40: GetSystemMetrics) before
     * setting the mode; edge scrolling (459250) stops there, so it must be the frame's. */
    void_input_set_screen_size(frame_w, modes[chosen_mode][1]);
    void_log("[stronghold] display mode %d (%dx%d); saved %u, requested %dx%d", chosen_mode,
        frame_w, modes[chosen_mode][1], saved, width, height);
    return (uint32_t)chosen_mode;
}

/* DirectDraw also enumerates the chosen mode's table size: the engine's
 * availability check (45E670 -> 458CC0) knows only those, and an unknown one
 * falls back to the largest listed mode. A wide frame replaces the width only
 * afterwards (stronghold_original_dimensions); SetDisplayMode takes any size. */
int stronghold_display_requested(int *width, int *height)
{
    if (!chosen_mode) return 0;
    *width = modes[chosen_mode][0]; *height = modes[chosen_mode][1];
    return 1;
}

/* 45E4A0 has stored the mode's size (ecx screen, ebx index) and next derives
 * pitch, buffer sizes, clip rectangles and the centered 800x600 origin. */
void stronghold_original_dimensions(uint32_t screen, uint32_t mode)
{
    if (custom_width && screen == SCREEN && mode == (uint32_t)chosen_mode) MEM32(screen + 0x18) = (uint32_t)custom_width;
}

/* The camera clamp (4B33B0) ends the per-mode view table (4B3EC0) and every
 * scroll: tile columns (+84) follow the width as the table does
 * (width / 32 + 5, doubled zoomed out); the rotation pivot (+A0) scales with
 * it. A value other than the last written is the table's own. */
void stronghold_original_view(uint32_t camera)
{
    static uint32_t pivot_written;
    if (!custom_width || camera != CAMERA) return;
    int zoomed_out = MEM32(camera + 0x8C) != 0;
    MEM32(camera + 0x84) = (uint32_t)(((custom_width + 31) / 32 + 5) << zoomed_out);
    uint32_t pivot = MEM32(camera + 0xA0);
    if (pivot != pivot_written) MEM32(camera + 0xA0) = pivot_written = pivot * (uint32_t)custom_width / modes[chosen_mode][0];
}

/* 4919E0 / 491BE0 subtract the mode's edge width from x (cdecl, x at [esp+4]). */
void stronghold_original_bar(uint32_t stack)
{
    if (custom_width) MEM32(stack + 4) -= (uint32_t)((custom_width - modes[chosen_mode][0]) / 2);
    MEM32(stack + 4) += (uint32_t)notice_margin();
}

/* 4A73E0 fills the mode's rectangle: x2 of 45FCB0(x1, y1, x2, y2, colour). */
void stronghold_original_fill(uint32_t stack)
{
    if (custom_width) MEM32(stack + 8) = (uint32_t)(custom_width - 1);
}

/* HUD side edges (42A000, 42AE60, 431FD0): each mode loads the stock pair of
 * its width, image 0 drawn at x 0 and image 1 at the panel's right edge. A
 * custom width takes the narrowest stock pair at least as wide; the clipped
 * blit (448E60) crops its outer side at the screen edges. */
static const struct { uint16_t width; uint32_t names[4]; } edge_sets[] = {
    /* left, right, military left, military right */
    {1024, {0x5580CCu, 0x55815Cu, 0x558304u, 0x5582ECu}}, {1280, {0x5580BCu, 0x55814Cu, 0x5582D4u, 0x5582BCu}},
    {1360, {0x55805Cu, 0x5580ECu, 0x5581B4u, 0x55819Cu}}, {1366, {0x55806Cu, 0x5580FCu, 0x5581E4u, 0x5581CCu}},
    {1440, {0x55809Cu, 0x55812Cu, 0x558274u, 0x55825Cu}}, {1600, {0x5580ACu, 0x55813Cu, 0x5582A4u, 0x55828Cu}},
    {1680, {0x55804Cu, 0x5580DCu, 0x558184u, 0x55816Cu}}, {1920, {0x55808Cu, 0x55811Cu, 0x558244u, 0x55822Cu}},
    {2560, {0x55807Cu, 0x55810Cu, 0x558214u, 0x5581FCu}}};
#define EDGE_SETS ((int)(sizeof edge_sets / sizeof edge_sets[0]))
static int edge_set(void)
{
    for (int i = 0; i < EDGE_SETS; ++i) if (edge_sets[i].width >= custom_width) return i;
    return EDGE_SETS - 1;
}
uint32_t stronghold_original_edge_name(uint32_t name)
{
    if (!custom_width) return name;
    for (int i = 0; i < EDGE_SETS; ++i) for (int k = 0; k < 4; ++k)
        if (edge_sets[i].names[k] == name) return edge_sets[edge_set()].names[k];
    return name;
}
/* At a call of 447DB0(image, x, y): 1 sends it to the clipped 448E60 instead. */
int stronghold_original_edge(uint32_t stack)
{
    if (!custom_width) return 0;
    uint32_t image = MEM32(stack);
    if (image > 1) return 0;
    MEM32(stack + 4) = (uint32_t)(image ? (custom_width + 800) / 2 : (custom_width - edge_sets[edge_set()].width) / 2);
    return 1;
}

/* ---------------------------------------------------------- interface layers
 * In a match the presenter receives the world and seven interface layers it
 * scales by the chosen size: 0 the bottom panel with what sits on it (hints,
 * the hovered building's name, boxes reaching into it; anchored at the bottom
 * centre), 1 top left, 4 bottom left, 3 top right, 5 top centre, 6 right
 * centre, and 2 centred overlays/dialogs above all (kept inside the frame).
 * These are the flag classes actually registered by 5525D0..5528D0; the
 * resource list's lower rows keep the same anchor as its upper rows. Overlay items, the UI
 * tree passes and the
 * panel overhang draw into raster-shaped layer buffers pre-filled with the
 * transparent colour; their opaque raster stores, GDI text and DirectDraw
 * blits report coverage (stronghold_display_written / _blended). Dialogs are
 * drawn into the world raster as usual, so their translucent backgrounds keep
 * the real map, and lifted out afterwards. */
typedef struct {
    uint32_t guest;                 /* raster-shaped redirect target, allocated on first use */
    uint16_t *pixels;               /* published, width x height */
    uint8_t *coverage, *capture;    /* published coverage; coverage of the open frame */
    int used, left, top, right, bottom, filled_x, filled_y, refills;
    stronghold_rect fill, shown, dirty;
} ui_layer;
static ui_layer layers[SH_UI_LAYERS];
static stronghold_rect top_fit;
static int resource_animation;
static void rect_union(stronghold_rect *, int, int, int, int);
static uint16_t *world_copy, row_pixels[2560];
static uint8_t row_coverage[2560];
static size_t capacity;
/* box: left, top, right, bottom, 1 if it reaches into the panel (layer 0) */
static int frame_width, frame_height, frame_open, world_frozen, current = -1, origin_x, origin_y, boxes, box[2][5];
static int frame_ready, frame_taken, published_width, published_height, published_band, published_used[SH_UI_LAYERS];
static uint32_t saved_pixels;
static uint16_t key = 0xF81F;
static float ui_scale = 1, published_scale = 1, published_anchor[2];
static float published_edge_anchor[SH_UI_LAYERS][2];
static int highlight[4], loupe_active;
static float loupe_x, loupe_y;
static pthread_mutex_t display_lock = PTHREAD_MUTEX_INITIALIZER;
int stronghold_display_capture_active;
int stronghold_display_clear_batch_active;

static int allocate(int width, int height)
{
    if (width < 800 || height < 600 || width > 2560 || height > 1600) return 0;
    size_t count = (size_t)width * height;
    if (count > capacity) {
        uint16_t *world = malloc(count * 2), *pixels[SH_UI_LAYERS] = {0};
        uint8_t *coverage[SH_UI_LAYERS] = {0}, *capture[SH_UI_LAYERS] = {0};
        int ok = world != NULL;
        for (int i = 0; i < SH_UI_LAYERS; ++i) {
            pixels[i] = malloc(count * 2); coverage[i] = malloc(count); capture[i] = malloc(count);
            ok = ok && pixels[i] && coverage[i] && capture[i];
        }
        if (!ok) {
            free(world);
            for (int i = 0; i < SH_UI_LAYERS; ++i) { free(pixels[i]); free(coverage[i]); free(capture[i]); }
            void_log("[stronghold] cannot allocate the interface layers");
            return 0;
        }
        pthread_mutex_lock(&display_lock);
        free(world_copy); world_copy = world;
        for (int i = 0; i < SH_UI_LAYERS; ++i) {
            free(layers[i].pixels); free(layers[i].coverage); free(layers[i].capture);
            layers[i].pixels = pixels[i]; layers[i].coverage = coverage[i]; layers[i].capture = capture[i];
        }
        capacity = count; frame_width = 0;
        pthread_mutex_unlock(&display_lock);
    }
    if (width != frame_width || height != frame_height) {
        /* Another row pitch: the published layers start over, the buffers are cleared whole. */
        pthread_mutex_lock(&display_lock);
        for (int i = 0; i < SH_UI_LAYERS; ++i) {
            memset(layers[i].coverage, 0, count); memset(layers[i].capture, 0, count);
            layers[i].shown = (stronghold_rect){0, 0, 0, 0}; layers[i].dirty = (stronghold_rect){0, 0, width, height};
            layers[i].filled_x = -1; layers[i].refills = 0;
        }
        frame_width = width; frame_height = height; frame_ready = 0;
        pthread_mutex_unlock(&display_lock);
    }
    return 1;
}

/* Frame size and raster origin, or 0 outside a usable match frame. */
static int frame_geometry(void)
{
    int w = (int)MEM32(SCREEN_WIDTH), h = (int)MEM32(SCREEN_HEIGHT);
    int x = (int32_t)MEM32(RASTER_ORIGIN), y = (int32_t)MEM32(RASTER_ORIGIN + 4);
    if (w < 800 || h < 600 || w > 2560 || h > 1600 || x < 0 || y < 0 || x + w > 4056 || y + h > 2076) return 0;
    origin_x = x; origin_y = y;
    return w == frame_width && h == frame_height;
}

static uint16_t *raster_row(uint32_t base, int y) { return GPTR(base + RASTER_PITCH * (uint32_t)(origin_y + y) + 2u * (uint32_t)origin_x); }

/* Overlay items and the overhang draw into a layer: the world raster pointer
 * and its DirectDraw surface (GDI text) point at the layer buffer meanwhile. */
static void open_layer(int group)
{
    ui_layer *l = &layers[group];
    if (current >= 0 || !frame_open) return;
    if (!l->guest && !(l->guest = void_vm_alloc(0, RASTER_BYTES, 0x3000, 4))) {
        void_log("[stronghold] interface layer %d: no guest memory", group);
        frame_open = 0;
        return;
    }
    if (!l->used) {
        /* Where the last frame drew is cleared; everything at another origin, and now and then. */
        stronghold_rect r = l->fill;
        if (origin_x != l->filled_x || origin_y != l->filled_y || ++l->refills >= 60) {
            r = (stronghold_rect){0, 0, frame_width, frame_height}; l->refills = 0;
        }
        for (int y = r.top; y < r.bottom; ++y) {
            uint16_t *row = raster_row(l->guest, y);
            for (int x = r.left; x < r.right; ++x) row[x] = key;
            memset(l->capture + (size_t)y * frame_width + r.left, 0, (size_t)(r.right - r.left));
        }
        l->filled_x = origin_x; l->filled_y = origin_y;
        l->left = frame_width; l->top = frame_height; l->right = l->bottom = 0;
        l->used = 1;
    }
    saved_pixels = MEM32(WORLD_PIXELS);
    if (!void_ddraw_redirect_pixels(MEM32(WORLD_SURFACE), l->guest)) {
        static int warned;
        if (!warned++) void_log("[stronghold] world surface busy: interface stays in the world");
        return;
    }
    MEM32(WORLD_PIXELS) = l->guest;
    current = group;
    stronghold_display_capture_active = 1;
}

static void close_layer(void)
{
    if (current < 0) return;
    MEM32(WORLD_PIXELS) = saved_pixels;
    void_ddraw_redirect_pixels(MEM32(WORLD_SURFACE), saved_pixels);
    current = -1;
    stronghold_display_capture_active = 0;
}

/* Coverage of stores into the open layer: opaque (background 0) or a blend
 * whose remaining background weight is background/255 (GDI text). */
void stronghold_display_blended(uint32_t address, unsigned bytes, unsigned background)
{
    if (current < 0 || !bytes || (bytes & 1u) || bytes > RASTER_PITCH) return;
    ui_layer *l = &layers[current];
    if (address < l->guest || (uint64_t)address + bytes > (uint64_t)l->guest + RASTER_BYTES) return;
    uint32_t offset = address - l->guest, row_byte = offset % RASTER_PITCH;
    if ((row_byte & 1u) || bytes > RASTER_PITCH - row_byte) return;
    int y = (int)(offset / RASTER_PITCH) - origin_y, x = (int)(row_byte / 2) - origin_x, right = x + (int)(bytes / 2);
    if (y < 0 || y >= frame_height) return;
    if (x < 0) x = 0;
    if (right > frame_width) right = frame_width;
    if (x >= right) return;
    if (current == 1 && !resource_animation) rect_union(&top_fit, x, y, right, y + 1);
    uint8_t *a = l->capture + (size_t)y * frame_width;
    if (background > 255) background = 255;
    for (int i = x; i < right; ++i) a[i] = (uint8_t)(255 - ((255 - a[i]) * background + 127) / 255);
    if (x < l->left) l->left = x;
    if (right > l->right) l->right = right;
    if (y < l->top) l->top = y;
    if (y >= l->bottom) l->bottom = y + 1;
}
void stronghold_display_written(uint32_t address, unsigned bytes) { stronghold_display_blended(address, bytes, 0); }
/* The arriving cart may leave the screen without shrinking the functional UI. */
void stronghold_original_resource_animation(int before) { resource_animation = before; }
void stronghold_display_clear_scope(int enter) { (void)enter; }
void stronghold_display_clear_begin(uint32_t context) { (void)context; }
void stronghold_display_clear_clipped(uint32_t result) { (void)result; }

/* The speed notice follows the frame's top-right corner, independently of dialogs. */
static void anchor_speed_notice(void)
{
    if (MEM32(SPEED_NOTICE + 8) == 24 && MEM32(SPEED_NOTICE + 0x1C) == 0)
        MEM32(SPEED_NOTICE + 0x1C) = 9;
}

/* 532F56, before the world pass: a match frame starts. */
void stronghold_original_layers(void)
{
    stronghold_perf_frame_begin();
    close_layer();
    frame_open = world_frozen = boxes = 0;
    top_fit = (stronghold_rect){0}; resource_animation = 0;
    anchor_speed_notice();
    if (!allocate((int)MEM32(SCREEN_WIDTH), (int)MEM32(SCREEN_HEIGHT)) || !frame_geometry()) return;
    key = MEM16(UI_KEY);
    for (int i = 0; i < SH_UI_LAYERS; ++i) layers[i].used = 0;
    frame_open = 1;
}

/* 490160 draws each anchored overlay (flags +1C: 1 top, 2 bottom; 4 left,
 * 8 right, else centered on the 800x600 box) through its own callback.
 * Consecutive overlays of one layer share its redirect, which ends with the
 * loop (stronghold_original_items_end); the empty callback needs none. */
#define OVERLAY_NONE 0x0045C870u
void stronghold_original_item(uint32_t item, int before)
{
    if (!frame_open || !before || MEM32(item + 0x18) == OVERLAY_NONE) return;
    uint32_t flags = MEM32(item + 0x1C);
    int group = (flags & 0xC) == 4 ? ((flags & 3) == 2 ? 4 : 1) :
                (flags & 0xC) == 8 ? ((flags & 3) == 1 ? 3 : 6) :
                (flags & 3) == 1 ? 5 : (flags & 3) == 2 ? 0 : 2;
    if (MEM32(item + 8) == 26 || MEM32(item + 8) == 27) group = 1;
    if (current == group) return;
    close_layer();
    if (frame_geometry()) open_layer(group);
}

/* 532F60, after 490160: the drag box and markers that follow draw into the world. */
void stronghold_original_items_end(void) { close_layer(); }

/* Before the dialogs (533122): the world raster now holds the finished world. */
void stronghold_original_freeze(void)
{
    if (!frame_open || world_frozen) return;
    close_layer();
    if (!frame_geometry()) { frame_open = 0; return; }
    for (int y = 0; y < frame_height; ++y)
        memcpy(world_copy + (size_t)y * frame_width, raster_row(MEM32(WORLD_PIXELS), y), (size_t)frame_width * 2);
    world_frozen = 1;
}

/* After 4912F0 drew its box (+2C id, -1 closed; +8/+C/+10/+14 frame rectangle). */
void stronghold_original_box(uint32_t object)
{
    if (!frame_open || !world_frozen || boxes >= 2 || MEM32(object + 0x2C) == UINT32_MAX) return;
    int x = (int32_t)MEM32(object + 8), y = (int32_t)MEM32(object + 12);
    int right = x + (int32_t)MEM32(object + 16), bottom = y + (int32_t)MEM32(object + 20);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > frame_width) right = frame_width;
    if (bottom > frame_height) bottom = frame_height;
    if (x >= right || y >= bottom) return;
    box[boxes][0] = x; box[boxes][1] = y; box[boxes][2] = right; box[boxes][3] = bottom;
    /* Centered dialogs end above the panel (48E2F0); one reaching into it belongs to it. */
    box[boxes][4] = bottom > frame_height - HUD_HEIGHT;
    ++boxes;
}

/* The UI tree passes (4BDBE0 at 533104, 533118) and the keyed HUD overhang
 * copy (448560: portrait, radar frame) draw what sits on the panel. */
void stronghold_original_panel(int begin)
{
    if (!frame_open) return;
    if (begin && frame_geometry()) open_layer(0); else close_layer();
}

static void rect_union(stronghold_rect *r, int left, int top, int right, int bottom)
{
    if (left >= right || top >= bottom) return;
    if (r->left >= r->right || r->top >= r->bottom) { *r = (stronghold_rect){left, top, right, bottom}; return; }
    if (left < r->left) r->left = left;
    if (top < r->top) r->top = top;
    if (right > r->right) r->right = right;
    if (bottom > r->bottom) r->bottom = bottom;
}

/* Copies the differing bytes of a row: the changed span [*first,*last), or 0. */
static int copy_changed(void *destination, const void *source, size_t n, size_t *first, size_t *last)
{
    uint8_t *d = destination; const uint8_t *s = source;
    size_t a = 0, b = n;
    uint64_t x, y;
#if defined(__aarch64__)
    while (a + 32 <= n) {
        uint8x16_t diff = vorrq_u8(veorq_u8(vld1q_u8(d+a), vld1q_u8(s+a)),
                                   veorq_u8(vld1q_u8(d+a+16), vld1q_u8(s+a+16)));
        if (vmaxvq_u8(diff)) break;
        a += 32;
    }
#endif
    while (a + 8 <= n && (memcpy(&x, d + a, 8), memcpy(&y, s + a, 8), x == y)) a += 8;
    while (a < n && d[a] == s[a]) a++;
    if (a == n) return 0;
    while (b >= a + 8 && (memcpy(&x, d + b - 8, 8), memcpy(&y, s + b - 8, 8), x == y)) b -= 8;
    while (d[b - 1] == s[b - 1]) b--;
    memcpy(d + a, s + a, b - a); *first = a; *last = b;
    return 1;
}

/* One layer's published rows: what its buffer captured; the lifted dialogs
 * above it (layer 2) or, for boxes reaching into the panel, below it (the
 * original copies the overhang after them); then the opaque panel band.
 * Only changed rows join the dirty rectangle the presenter uploads. */
static void publish_layer(int i, int band)
{
    ui_layer *l = &layers[i];
    int w = frame_width, h = frame_height;
    stronghold_rect area = {0, 0, 0, 0};
    if (l->used) rect_union(&area, l->left, l->top, l->right, l->bottom);
    for (int b = 0; b < boxes; ++b) if (i == (box[b][4] ? 0 : 2)) rect_union(&area, box[b][0], box[b][1], box[b][2], box[b][3]);
    if (i == 0 && band) rect_union(&area, 0, h - HUD_BAND, w, h);
    stronghold_rect shown = area;
    rect_union(&area, l->shown.left, l->shown.top, l->shown.right, l->shown.bottom);
    int first = area.bottom, last = area.top, left = area.right, right = area.left;
    size_t n = (size_t)(area.right - area.left), a, b;
    uint32_t ui = MEM32(UI_PIXELS), ui_pitch = MEM32(UI_PITCH);
    int band_rows = i == 0 && band && ui && ui_pitch >= (uint32_t)w * 2;
    for (int y = area.top; y < area.bottom; ++y) {
        uint16_t *pixels = row_pixels + area.left; uint8_t *coverage = row_coverage + area.left;
        if (band_rows && y >= h - HUD_BAND) {
            /* Boxes reaching into the panel are drawn before the overhang the band shows. */
        } else if (l->used && y >= l->top && y < l->bottom) {
            memcpy(pixels, raster_row(l->guest, y) + area.left, n * 2);
            memcpy(coverage, l->capture + (size_t)y * w + area.left, n);
        } else {
            for (size_t x = 0; x < n; ++x) pixels[x] = key;
            memset(coverage, 0, n);
        }
        for (int k = 0; k < boxes; ++k) if (i == (box[k][4] ? 0 : 2) && y >= box[k][1] && y < box[k][3]) {
            int x0 = box[k][0] > area.left ? box[k][0] : area.left, x1 = box[k][2] < area.right ? box[k][2] : area.right;
            const uint16_t *lifted = raster_row(MEM32(WORLD_PIXELS), y);
            if (i == 2) {
                if (x0 < x1) memcpy(row_pixels + x0, lifted + x0, (size_t)(x1 - x0) * 2);
                if (x0 < x1) memset(row_coverage + x0, 255, (size_t)(x1 - x0));
                continue;
            }
            for (int x = x0; x < x1; ++x) {
                unsigned a = row_coverage[x];
                if (a == 255) continue;
                row_pixels[x] = a ? stronghold_composite_pixel(row_pixels[x], lifted[x], a, key) : lifted[x];
                row_coverage[x] = 255;
            }
        }
        if (band_rows && y >= h - HUD_BAND) {
            memcpy(pixels, (const uint16_t *)GPTR(ui + ui_pitch * (uint32_t)y) + area.left, n * 2);
            memset(coverage, 255, n);
        }
        int changed = 0;
        if (copy_changed(l->pixels + (size_t)y * w + area.left, pixels, n * 2, &a, &b)) {
            changed = 1; if ((int)(a / 2) < left - area.left) left = area.left + (int)(a / 2);
            if ((int)((b + 1) / 2) > right - area.left) right = area.left + (int)((b + 1) / 2);
        }
        if (copy_changed(l->coverage + (size_t)y * w + area.left, coverage, n, &a, &b)) {
            changed = 1; if ((int)a < left - area.left) left = area.left + (int)a;
            if ((int)b > right - area.left) right = area.left + (int)b;
        }
        if (changed) { if (y < first) first = y; last = y + 1; }
    }
    if (first < last) rect_union(&l->dirty, left, first, right, last);
    l->shown = shown;
    /* Stores of an unused layer's last frame are still in its buffer. */
    if (l->used) l->fill = (stronghold_rect){l->left, l->top, l->right, l->bottom};
}

/* An anchor that scales [lo, hi) by s about its own centre, shifted to stay
 * inside [0, limit) (centered when it cannot fit). */
static float fit_anchor(int lo, int hi, int limit, float s)
{
    float c = (lo + hi) * .5f, half = (hi - lo) * .5f * s, target = c;
    if (half * 2 >= limit) target = limit * .5f;
    else if (target - half < 0) target = half;
    else if (target + half > limit) target = limit - half;
    return s > 1.001f ? (target - c * s) / (1 - s) : c;
}

/* Keep each original edge anchor, moving it only far enough for the captured
 * content to fit. A centre-top notice must not inherit a modal's transform. */
static float fit_edge_anchor(int lo, int hi, int limit, float s, float anchor)
{
    float low = anchor + (lo - anchor) * s, high = anchor + (hi - anchor) * s;
    float shift = low < 0 ? -low : high > limit ? limit - high : 0;
    return s > 1.001f ? anchor + shift / (1 - s) : anchor;
}

/* The book extends 80px into edge*r.tgx beyond the 800px HUD box: its
 * farthest edge is 480px from centre. Only the repeating side art may crop. */
static void publish_geometry(int band)
{
    float s = ui_scale;
    if (band) s = fminf(s, frame_width / 960.f);
    if (published_used[0]) {
        int span = frame_height - layers[0].shown.top;
        if (span > 0) s = fminf(s, (float)frame_height / span);
        if (span > 0 && published_used[1] && top_fit.left < top_fit.right && top_fit.bottom > top_fit.top) {
            /* The same requested scale must fit the complete top-left
             * resource list above the functional bottom panel. */
            int total = top_fit.bottom + span;
            if (total > 0) s = fminf(s, (float)frame_height / total);
        }
    }
    for (int b = 0; b < boxes; ++b) if (box[b][4]) {
        float half = fmaxf(frame_width * .5f - box[b][0], box[b][2] - frame_width * .5f);
        if (half > 0) s = fminf(s, frame_width * .5f / half);
    }
    stronghold_rect fit = {0, 0, 0, 0};
    for (int b = 0; b < boxes; ++b) if (!box[b][4]) rect_union(&fit, box[b][0], box[b][1], box[b][2], box[b][3]);
    if (fit.left >= fit.right) fit = layers[2].shown;
    if (fit.left < fit.right && fit.top < fit.bottom) {
        s = fminf(s, (float)frame_width / (fit.right - fit.left));
        s = fminf(s, (float)frame_height / (fit.bottom - fit.top));
    }
    for (int i = 1; i < SH_UI_LAYERS; ++i) if (i != 2 && published_used[i]) {
        stronghold_rect b = i == 1 ? top_fit : layers[i].shown;
        if (b.left < b.right) s = fminf(s, (float)frame_width / (b.right - b.left));
        if (b.top < b.bottom) s = fminf(s, (float)frame_height / (b.bottom - b.top));
    }
    published_scale = fmaxf(1.f, s);
    published_anchor[0] = fit_anchor(fit.left, fit.right, frame_width, published_scale);
    published_anchor[1] = fit_anchor(fit.top, fit.bottom, frame_height, published_scale);
    for (int i = 1; i < SH_UI_LAYERS; ++i) if (i != 2) {
        stronghold_rect b = i == 1 ? top_fit : layers[i].shown;
        float ax = i == 3 || i == 6 ? frame_width : i == 5 ? frame_width * .5f : 0;
        float ay = i == 4 ? frame_height : i == 6 ? frame_height * .5f : 0;
        published_edge_anchor[i][0] = b.left < b.right ? fit_edge_anchor(b.left, b.right, frame_width, published_scale, ax) : ax;
        published_edge_anchor[i][1] = b.top < b.bottom ? fit_edge_anchor(b.top, b.bottom, frame_height, published_scale, ay) : ay;
    }
}

/* 533156, before the present (45EC40) blits the frame: publish it. */
void stronghold_original_present(void)
{
    if (frame_open) stronghold_original_freeze();
    close_layer();
    if (!frame_open || !world_frozen) {
        /* Menus, briefings and videos present their own frame. */
        pthread_mutex_lock(&display_lock); frame_ready = 0; pthread_mutex_unlock(&display_lock);
        frame_open = 0;
        return;
    }
    frame_open = 0;
    int band = MEM32(PRESENT_MODE) == 1;
    pthread_mutex_lock(&display_lock);
    for (int i = 0; i < SH_UI_LAYERS; ++i) publish_layer(i, band);
    for (int i = 0; i < SH_UI_LAYERS; ++i) published_used[i] = layers[i].shown.left < layers[i].shown.right;
    published_width = frame_width; published_height = frame_height; published_band = band;
    publish_geometry(band);
    frame_ready = 1; frame_taken = 0;
    pthread_mutex_unlock(&display_lock);
}

/* Save callbacks present between match frames; retain the complete panel overhang. */
void stronghold_original_progress(int bar)
{
    if(frame_open||!frame_ready||!published_band||!published_used[0]||
       !frame_geometry()||MEM32(DIALOG)!=14)return;
    int x=bar?(int32_t)MEM32(0xBC734Cu):(int32_t)MEM32(0x1503CC0u);
    int y=bar?(int32_t)MEM32(0xBC7350u):(int32_t)MEM32(0x1503CC4u);
    int w=bar?301:(int32_t)MEM32(0x1503CC8u),h=bar?16:(int32_t)MEM32(0x1503CCCu);
    if(x<0||y<0||w<=0||h<=0||w>frame_width-x||h>frame_height-y||
       !void_vm_readable(MEM32(WORLD_PIXELS),RASTER_BYTES))return;
    pthread_mutex_lock(&display_lock);
    boxes=0;publish_layer(0,1);
    ui_layer *l=&layers[2];stronghold_rect old=l->shown;
    if(!bar){
        for(int row=old.top;row<old.bottom;row++)
            memset(l->coverage+(size_t)row*frame_width+old.left,0,(size_t)(old.right-old.left));
        l->shown=(stronghold_rect){0};
    }
    for(int row=y;row<y+h;row++){
        memcpy(l->pixels+(size_t)row*frame_width+x,raster_row(MEM32(WORLD_PIXELS),row)+x,(size_t)w*2);
        memset(l->coverage+(size_t)row*frame_width+x,255,(size_t)w);
    }
    rect_union(&l->shown,x,y,x+w,y+h);rect_union(&l->dirty,old.left,old.top,old.right,old.bottom);
    rect_union(&l->dirty,x,y,x+w,y+h);
    for(int i=1;i<SH_UI_LAYERS;i++)published_used[i]=i==2;
    published_anchor[0]=fit_anchor(l->shown.left,l->shown.right,frame_width,published_scale);
    published_anchor[1]=fit_anchor(l->shown.top,l->shown.bottom,frame_height,published_scale);
    frame_taken=0;
    pthread_mutex_unlock(&display_lock);
}

/* --------------------------------------------- presentation and the layers */
static int filter_mode = 2, surface_width, surface_height;
static float filter_sharpness = .25f;
static float cutout_left;
void stronghold_display_cutout(float left)
{
    if (!isfinite(left) || left < 0 || left > .5f) return;
    pthread_mutex_lock(&display_lock); cutout_left = left; pthread_mutex_unlock(&display_lock);
}
static int notice_margin(void)
{
    pthread_mutex_lock(&display_lock);
    int w = (int)MEM32(SCREEN_WIDTH), h = (int)MEM32(SCREEN_HEIGHT);
    int x = stronghold_notice_margin(w,h,surface_width,surface_height,cutout_left,1);
    pthread_mutex_unlock(&display_lock); return x;
}
void stronghold_display_powers_scale(float scale) { (void)scale; }
static int filter_world = SH_FILTER_FSR, filter_ui = SH_FILTER_FSR, filter_deband, filter_crisp = 1;

int stronghold_display_dimensions(uint32_t screen) { (void)screen; return 0; }
uint32_t stronghold_display_width(uint32_t original) { return original; }
uint32_t stronghold_display_height(uint32_t original) { return original; }
uint32_t stronghold_display_half_width(void) { return 240; }
void stronghold_display_viewport(uint32_t map) { (void)map; }
void stronghold_display_clear(uint32_t stack) { (void)stack; }
uint32_t stronghold_display_edge_name(uint32_t name) { return name; }
int stronghold_display_hud_edge(uint32_t renderer, uint32_t stack, int right) { (void)renderer; (void)stack; (void)right; return 0; }
void stronghold_display_hud_clip(uint32_t renderer, uint32_t saved[4], int restore) { (void)renderer; (void)saved; (void)restore; }
/* The panel and the dialogs grow from 100% to 200% (a smaller panel would
 * uncover map rows the original still treats as its own). */
void stronghold_display_set_ui_scale(float scale)
{
    if (!isfinite(scale)) return;
    pthread_mutex_lock(&display_lock);
    ui_scale = fmaxf(1.f, fminf(2.f, scale));
    pthread_mutex_unlock(&display_lock);
}
void stronghold_display_set_render_scale(float scale) { (void)scale; }
float stronghold_display_world_zoom(void) { return 1; }
void stronghold_display_transform(float nx, float ny, float ndx, float ndy, float factor) { (void)nx; (void)ny; (void)ndx; (void)ndy; (void)factor; }
void stronghold_display_zoom_session(void) {}
void stronghold_display_zoom_reset(void) {}
void stronghold_display_highlight(int left, int top, int width, int height)
{
    pthread_mutex_lock(&display_lock);
    highlight[0] = left; highlight[1] = top;
    highlight[2] = width > 0 ? width : 0; highlight[3] = height > 0 ? height : 0;
    pthread_mutex_unlock(&display_lock);
}
/* The presentation filters are generic GPU passes: the world and the
 * interface layers have their own; menus use the interface filter. */
void stronghold_display_set_filters(int world, int ui, float sharpness, int deband, int crisp_text)
{
    pthread_mutex_lock(&display_lock);
    filter_world = world >= SH_FILTER_BILINEAR && world <= SH_FILTER_FSR ? world : SH_FILTER_FSR;
    filter_ui = ui >= SH_FILTER_BILINEAR && ui <= SH_FILTER_FSR ? ui : SH_FILTER_FSR;
    filter_mode = filter_world == SH_FILTER_FSR ? 2 : filter_world;
    filter_sharpness = sharpness < 0 ? 0 : sharpness > 1 ? 1 : sharpness;
    filter_deband = deband != 0;
    filter_crisp = crisp_text != 0;
    pthread_mutex_unlock(&display_lock);
}
void stronghold_display_filters(int *world, int *ui, float *sharpness, int *deband, int *crisp_text)
{
    pthread_mutex_lock(&display_lock);
    if (world) *world = filter_world;
    if (ui) *ui = filter_ui;
    if (sharpness) *sharpness = filter_sharpness;
    if (deband) *deband = filter_deband;
    if (crisp_text) *crisp_text = filter_crisp;
    pthread_mutex_unlock(&display_lock);
}
void stronghold_display_set_image_filter(int mode, float sharpness)
{
    int both = mode >= SH_FILTER_BILINEAR && mode <= SH_FILTER_FSR ? mode : SH_FILTER_FSR;
    int deband, crisp;
    stronghold_display_filters(NULL, NULL, NULL, &deband, &crisp);
    stronghold_display_set_filters(both, both, sharpness, deband, crisp);
}
int stronghold_display_image_filter(float *sharpness)
{
    pthread_mutex_lock(&display_lock);
    *sharpness = filter_sharpness; int mode = filter_mode;
    pthread_mutex_unlock(&display_lock); return mode;
}
void stronghold_display_magnifier(int active, float nx, float ny)
{
    pthread_mutex_lock(&display_lock);
    loupe_active = active && isfinite(nx) && isfinite(ny);
    if (loupe_active) { loupe_x = fmaxf(0, fminf(1, nx)); loupe_y = fmaxf(0, fminf(1, ny)); }
    pthread_mutex_unlock(&display_lock);
}
int stronghold_display_loupe(float *nx, float *ny)
{
    pthread_mutex_lock(&display_lock);
    int active = loupe_active;
    if (active) { *nx = loupe_x; *ny = loupe_y; }
    pthread_mutex_unlock(&display_lock);
    return active;
}

/* One part per original anchor class. In particular, resource rows beyond
 * half the screen remain in the same top-left transform as their heading. */
static int layer_parts(stronghold_part part[SH_UI_PARTS], int w, int h)
{
    int n = 0;
    for (int p = 0; p < SH_UI_LAYERS; ++p) {
        int i = SH_UI_ORDER[p];
        if (!published_used[i]) continue;
        float ax = i == 0 ? w * .5f : i == 2 ? published_anchor[0] : published_edge_anchor[i][0];
        float ay = i == 0 ? h : i == 2 ? published_anchor[1] : published_edge_anchor[i][1];
        part[n++] = (stronghold_part){i, 0, h, ax, ay};
    }
    return n;
}

/* The frame of a match: world and layers. Its present (45EC40) blits the
 * world rows and changed panel rectangles one by one; the first presents it
 * all, the others are skipped. Menus and videos present their frame as is. */
int stronghold_display_frame(stronghold_frame *frame, int width, int height, int bpp)
{
    pthread_mutex_lock(&display_lock);
    if (!frame_ready || width != published_width || height != published_height || bpp != 16) {
        frame_ready = 0;
        pthread_mutex_unlock(&display_lock);
        return 0;
    }
    if (frame_taken) { pthread_mutex_unlock(&display_lock); return -1; }
    memset(frame, 0, sizeof *frame);
    frame->width = width; frame->height = height; frame->world = world_copy;
    frame->raster_width = width; frame->raster_height = height;
    frame->world_scale_x = frame->world_scale_y = 1;
    frame->ui_scale = published_scale; frame->key = key;
    frame->fresh = 1; frame->hud_opaque = published_band;
    for (int i = 0; i < SH_UI_LAYERS; ++i) {
        frame->layer[i].pixels = layers[i].pixels; frame->layer[i].coverage = layers[i].coverage;
        frame->layer[i].used = published_used[i];
        frame->layer[i].bounds = layers[i].shown; frame->layer[i].dirty = layers[i].dirty;
        layers[i].dirty = (stronghold_rect){0, 0, 0, 0};
    }
    frame->parts = layer_parts(frame->part, width, height);
    if (highlight[2] > 0 && highlight[3] > 0 && published_used[0])
        frame->highlight = (stronghold_rect){highlight[0], highlight[1], highlight[0] + highlight[2], highlight[1] + highlight[3]};
    frame_taken = 1;
    pthread_mutex_unlock(&display_lock);
    return 1;
}
int stronghold_display_locate(uint32_t address, int *x, int *y)
{
    if (address & 1u) return -1;
    for (int i = 0; i < SH_UI_LAYERS; ++i) if (layers[i].guest && address >= layers[i].guest &&
            (uint64_t)address < (uint64_t)layers[i].guest + RASTER_BYTES) {
        uint32_t offset = address - layers[i].guest;
        *x = (int)(offset % RASTER_PITCH / 2) - origin_x; *y = (int)(offset / RASTER_PITCH) - origin_y;
        return i;
    }
    /* The persistent UI surface is also the frame of menus before a match. */
    uint32_t ui = MEM32(UI_PIXELS), pitch = MEM32(UI_PITCH), height = MEM32(SCREEN_HEIGHT);
    if (ui && pitch >= 2 && height && address >= ui && (uint64_t)address < (uint64_t)ui + (uint64_t)pitch * height) {
        uint32_t offset = address - ui;
        *x = (int)(offset % pitch / 2); *y = (int)(offset / pitch);
        return SH_UI_LAYERS;
    }
    return -1;
}
int stronghold_display_crisp_text(void)
{
    pthread_mutex_lock(&display_lock); int on = filter_crisp;
    pthread_mutex_unlock(&display_lock); return on;
}
const uint8_t *stronghold_display_capture_row(int *group, int y, int *width)
{
    *group = -1;
    if (!frame_open || !stronghold_display_capture_active || current < 0 || y < 0 || y >= frame_height || !layers[current].capture) return NULL;
    *group = current; *width = frame_width;
    return layers[current].capture + (size_t)y * frame_width;
}
int stronghold_display_text_layers(uint16_t *layers_out[SH_UI_LAYERS], uint8_t *coverage[SH_UI_LAYERS],
                                    int bounds[SH_UI_LAYERS][4], int *width, int *height)
{
    if (frame_open || !frame_ready || !published_width) return 0;
    for (int i = 0; i < SH_UI_LAYERS; ++i) {
        layers_out[i] = published_used[i] ? layers[i].pixels : NULL;
        coverage[i] = published_used[i] ? layers[i].coverage : NULL;
        stronghold_rect r = layers[i].shown;
        bounds[i][0] = r.left; bounds[i][1] = r.top; bounds[i][2] = r.right; bounds[i][3] = r.bottom;
    }
    *width = published_width; *height = published_height; return 1;
}
int stronghold_display_layer_view(int group, float lx, float ly, float *vx, float *vy, float *scale)
{
    if (group < -1 || group >= SH_UI_LAYERS || !isfinite(lx) || !isfinite(ly)) return 0;
    pthread_mutex_lock(&display_lock);
    /* Android positions controls before the guest image has committed its screen. */
    if (!frame_ready && !void_vm_readable(SCREEN_WIDTH, 8)) {
        pthread_mutex_unlock(&display_lock); return 0;
    }
    int w = frame_ready ? published_width : (int)MEM32(SCREEN_WIDTH), h = frame_ready ? published_height : (int)MEM32(SCREEN_HEIGHT);
    int sw = surface_width > 0 ? surface_width : w, sh = surface_height > 0 ? surface_height : h;
    int ok = w > 0 && h > 0 && sw > 0 && sh > 0;
    double x = lx, y = ly, s = 1;
    if (ok && group >= 0) {
        stronghold_part parts[SH_UI_PARTS]; int found = 0;
        int n = frame_ready ? layer_parts(parts, w, h) : 0;
        for (int p = 0; p < n; ++p) if (parts[p].layer == group && ly >= parts[p].top &&
                (ly < parts[p].bottom || (ly == h && parts[p].bottom == h))) {
            s = published_scale;
            x = parts[p].anchor_x + (lx - parts[p].anchor_x) * s;
            y = parts[p].anchor_y + (ly - parts[p].anchor_y) * s;
            found = 1; break;
        }
        ok = found;
    }
    if (ok) {
        double fit = fmin((double)sw / w, (double)sh / h);
        *vx = (float)((sw - w * fit) * .5 + x * fit);
        *vy = (float)((sh - h * fit) * .5 + y * fit); *scale = (float)(s * fit);
    }
    pthread_mutex_unlock(&display_lock); return ok;
}

/* A surface point as a frame point, inverting the presenter's aspect fit. */
static void surface_frame(float nx, float ny, int w, int h, double *fx, double *fy)
{
    double sw = surface_width > 0 ? surface_width : w, sh = surface_height > 0 ? surface_height : h;
    double fit = fmin(sw / w, sh / h);
    *fx = (nx * sw - (sw - w * fit) * .5) / fit; *fy = (ny * sh - (sh - h * fit) * .5) / fit;
}
/* Where a layer pixel shows: around the part's anchor at the published scale. */
static void part_source(const stronghold_part *p, double fx, double fy, double s, double *sx, double *sy)
{
    *sx = p->anchor_x + (fx - p->anchor_x) / s; *sy = p->anchor_y + (fy - p->anchor_y) / s;
}
/* The topmost covered layer pixel under a frame point: its part, or -1 for the world. */
static int pick_part(double fx, double fy, stronghold_part *hit)
{
    if (!frame_ready) return -1;
    stronghold_part part[SH_UI_PARTS];
    int n = layer_parts(part, published_width, published_height);
    for (int p = n - 1; p >= 0; --p) {
        double sx, sy;
        part_source(&part[p], fx, fy, published_scale, &sx, &sy);
        int x = (int)floor(sx), y = (int)floor(sy);
        if (x < 0 || y < part[p].top || x >= published_width || y >= part[p].bottom) continue;
        if (layers[part[p].layer].coverage[(size_t)y * published_width + x]) { *hit = part[p]; return p; }
    }
    return -1;
}
int stronghold_display_map_point(float nx, float ny, int *gx, int *gy)
{
    if (!isfinite(nx) || !isfinite(ny)) return 0;
    pthread_mutex_lock(&display_lock);
    int ok = frame_ready;
    if (ok) {
        double fx, fy, sx, sy;
        stronghold_part hit;
        surface_frame(nx, ny, published_width, published_height, &fx, &fy);
        if (pick_part(fx, fy, &hit) >= 0) part_source(&hit, fx, fy, published_scale, &fx, &fy);
        *gx = (int)floor(fx); *gy = (int)floor(fy);
    }
    pthread_mutex_unlock(&display_lock);
    return ok;
}
int stronghold_display_world_point(float nx, float ny, int *gx, int *gy) { (void)nx; (void)ny; (void)gx; (void)gy; return 0; }
int stronghold_display_ui_point(int group, float nx, float ny, int *gx, int *gy) { (void)group; (void)nx; (void)ny; (void)gx; (void)gy; return 0; }
float stronghold_display_ui_dy(float normalized_dy) { (void)normalized_dy; return 0; }
float stronghold_display_ui_dx(float normalized_dx) { (void)normalized_dx; return 0; }
void stronghold_display_surface_size(int width, int height) { surface_width = width; surface_height = height; }
void stronghold_display_frame_begin(void) {}
void stronghold_display_world_begin(uint32_t map) { (void)map; }
void stronghold_display_world_render(uint32_t map) { (void)map; }
uint32_t stronghold_display_world_width(void) { return 0; }
uint32_t stronghold_display_world_height(void) { return 0; }
void stronghold_display_ui_begin(uint32_t map) { (void)map; }
void stronghold_display_ui_group(uint32_t root, int enter) { (void)root; (void)enter; }
void stronghold_display_ui_end(void) {}
const uint8_t *stronghold_display_compose(const uint8_t *pixels, int width, int height, int *pitch, int bpp)
{ (void)width; (void)height; (void)pitch; (void)bpp; return pixels; }

/* ------------------------------------------------------ game UI (inert) */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int in_game(void)
{
    if (!void_vm_readable(STATE, 4) || !void_vm_readable(DIALOG, 4)) return 0;  /* before the image maps */
    uint32_t state = MEM32(STATE);
    return state == 12 || state == 14 || state == 16;
}
/* [0]: 1 in a match, 8 the main menu without a dialog (the game switch shows);
 * [4..7]: frame size and the HUD's 800x600 box origin. */
void stronghold_game_control_state(int *values)
{
    memset(values, 0, SH_CONTROL_STATE * sizeof *values);
    if (!void_vm_readable(STATE, 4) || !void_vm_readable(DIALOG, 4)) return;
    values[0] = in_game() ? 1 : MEM32(STATE) == 41 && MEM32(DIALOG) == UINT32_MAX ? 8 : 0;
    if (values[0] & 1) {
        if (MEM32(0x1503D84u) == 1) values[0] |= 2;
        uint32_t units = 0xD47148u, count = MEM32(units), player = MEM32(0x1008144u);
        if (count <= 15001) {
            for (uint32_t id = 1; id < count; ++id) {
                uint32_t u = units + id * 1044u;
                if (MEM16(u + 716) && MEM16(u + 804) == 2 && !MEM16(u + 1336) &&
                    MEM16(u + 806) != 55 && MEM16(u + 814) == player) ++values[1];
            }
            for (unsigned slot = 1; slot <= 9; ++slot) {
                uint32_t group = 0xBCAFB8u + slot * 12000u;
                for (unsigned n = 0; n < 1500; ++n) {
                    uint32_t id = MEM32(group + n * 8u);
                    if (!id || id >= count) continue;
                    uint32_t u = units + id * 1044u;
                    if (MEM32(u + 816) == MEM32(group + n * 8u + 4) && MEM16(u + 804) == 2 &&
                        !MEM16(u + 1336) && MEM16(u + 814) == player) {
                        values[2] |= 1 << (slot - 1); break;
                    }
                }
            }
        }
        for (unsigned slot = 1; slot <= 9; ++slot)
            if ((int32_t)MEM32(0xBF5628u + ('0' + slot) * 4u) > 0) values[3] |= 1 << (slot - 1);
        values[4] = (int)MEM32(SCREEN_WIDTH); values[5] = (int)MEM32(SCREEN_HEIGHT);
        values[6] = (int32_t)MEM32(BOX_ORIGIN); values[7] = (int32_t)MEM32(BOX_ORIGIN + 4);
        values[8] = (stronghold_original_radar_colors() ? 1 : 0) | (MEM32(0x1503B0Cu) ? 2 : 0);
        values[9] = (int)MEM32(0x1503DE4u);
    }
}
/* Android shortcuts become the original keys: Space, Z zoom, X/C turn, digits. */
static void tap_key(uint32_t vk, int modifiers)
{
    if (modifiers & 1) void_input_key(0x11, 1);
    if (modifiers & 2) void_input_key(0x12, 1);
    void_input_key(vk, 1); void_input_key(vk, 0);
    if (modifiers & 2) void_input_key(0x12, 0);
    if (modifiers & 1) void_input_key(0x11, 0);
}
static int command_keys[16], command_modifiers[16], command_count;
/* The semantic input switch already owns lock; public JNI callers do not. */
static void command_locked(int key,int modifiers)
{
    if(command_count<16){command_keys[command_count]=key;command_modifiers[command_count++]=modifiers;}
}
void stronghold_game_command(int key, int modifiers)
{
    if (modifiers < 0 || modifiers > 3 || !((key >= '0' && key <= '9') || key == 0x20 || key == 'Z' ||
        key == 'X' || key == 'C' || key == 'R' || key == 'H' || key == 'V' || key == 'E' || key == 'D' ||
        key == 0xDE || key == 0xBC || key == 0x6B || key == 0x6D)) return;
    pthread_mutex_lock(&lock);
    command_locked(key,modifiers);
    pthread_mutex_unlock(&lock);
}
static void original_commands(void)
{
    int keys[16], modifiers[16], count;
    pthread_mutex_lock(&lock);
    count = command_count; command_count = 0;
    memcpy(keys, command_keys, sizeof keys); memcpy(modifiers, command_modifiers, sizeof modifiers);
    pthread_mutex_unlock(&lock);
    if (!in_game() || MEM32(DIALOG) != UINT32_MAX) return;
    for (int i = 0; i < count; ++i) {
        if(keys[i]=='R'&&modifiers[i]==2){stronghold_original_radar_toggle();continue;}
        const uint32_t ctrl = 0xAB49BCu, shift = ctrl + 4, alt = ctrl + 8;
        uint32_t saved[] = {MEM32(ctrl), MEM32(shift), MEM32(alt)};
        MEM32(ctrl) = !!(modifiers[i] & 1); MEM32(shift) = 0; MEM32(alt) = !!(modifiers[i] & 2);
        int key = keys[i];
        uint32_t args[] = {MEM32(SCREEN + 0xB0), modifiers[i] & 2 ? 0x104u : 0x100u, (uint32_t)key, 1};
        void_regs registers; void_regs_save(&registers);
        void_call_guest(0x4923D0u, 4, args, 1); void_regs_load(&registers);
        MEM32(ctrl) = saved[0]; MEM32(shift) = saved[1]; MEM32(alt) = saved[2];
    }
}
static int ui_events;
static int skip_startup, skipped_startup;
void stronghold_game_tools(void) { pthread_mutex_lock(&lock);ui_events|=4;pthread_mutex_unlock(&lock); }
void stronghold_game_ui_frame(void)
{
    if(!void_vm_readable(STATE,4)||!void_vm_readable(DIALOG,4))return;
    if(skip_startup&&!skipped_startup&&(MEM32(STATE)==3||MEM32(STATE)==26)) {
        const uint32_t args[]={48,0};void_regs saved;void_regs_save(&saved);
        void_call_guest_this(0x45BEB0u,0x1503D30u,2,args);void_regs_load(&saved);
        skipped_startup=1;void_log("[startup] skipping HD logos and intro");
    }
    int event=stronghold_original_options_frame();
    event|=stronghold_text_frame(MEM32(STATE),MEM32(DIALOG));
    if(event){pthread_mutex_lock(&lock);ui_events|=event;pthread_mutex_unlock(&lock);}
}
void stronghold_game_ui_skip(int enabled) { skip_startup=enabled; }
int stronghold_game_ui_event(void)
{
    pthread_mutex_lock(&lock);
    int events = ui_events; ui_events = 0;
    pthread_mutex_unlock(&lock);
    return events;
}

/* --------------------------------------------- generated seams (unused) */
int stronghold_touch_callback(int kind) { (void)kind; return 0; }
uint32_t stronghold_touch_legacy(uint32_t original) { return original; }
uint32_t stronghold_touch_x(uint32_t original) { return original; }
uint32_t stronghold_touch_y(uint32_t original) { return original; }
uint32_t stronghold_touch_ui_x(uint32_t original) { return original; }
uint32_t stronghold_touch_ui_y(uint32_t original) { return original; }
uint32_t stronghold_touch_ui_button(uint32_t original) { return original; }
int stronghold_touch_ui_redraw(void) { return stronghold_original_build_redraw(); }
uint32_t stronghold_touch_ui_help(uint32_t original) { return stronghold_original_build_help(original); }
int stronghold_touch_selection(void) { return 0; }
int stronghold_touch_group_unfiltered(void) { return 0; }
int stronghold_touch_lord_control(void) { return 0; }
int stronghold_touch_order(int context) { (void)context; return 0; }
int stronghold_touch_ability(void) { return 0; }
int stronghold_touch_cancel(void) { return 0; }
int stronghold_touch_inspect(void) { return 0; }
int stronghold_touch_picker(void) { return 0; }
uint32_t stronghold_touch_placement(int predicate, uint32_t original) { (void)predicate; return original; }
int stronghold_touch_known(void) { return 0; }
int stronghold_touch_selftest(void) { return 0; }
void stronghold_touch_frame(void) {}
void stronghold_touch_after_world(void) {}

/* ------------------------------------------------------ direct mouse touch
 * Menus, dialogs and the bottom panel are UI: a tap clicks, a drag holds the
 * left button, a long press right-clicks. On the map a quick drag moves the
 * camera like the original scroll code, a hold-then-drag drags the left
 * button (selection, walls), a long press or a two-finger tap right-clicks,
 * a pinch toggles the original zoom (Z) and a twist turns the map (C/X).
 * JNI calls run on the Android UI thread: they only post input or record a
 * camera delta; the guest is changed on its own thread (stronghold_original_frame).
 *
 * The original samples its button state once per frame (459250) and acts on
 * the edges, so a press must span frames as a physical click does: a release
 * waits for two frame seams and 50 ms (400 ms while the loop is blocked),
 * and a drag holds the cursor at its start until the press was sampled.
 * A touch that starts on a scaled interface layer stays mapped through that
 * layer until it ends; one that starts on the map stays on the map's pixels. */
static unsigned current_sequence;
static int current_kind, held, touch_direct = 1, touch_layer;
static uint32_t aim_build;
static stronghold_part touch_part;
static int start_x, start_y;
static float start_nx, start_ny, pan_x, pan_y, pinch = 1, touch_scale = 1;
static int smooth_camera = 1;

void stronghold_display_smooth_camera(int enabled)
{
    pthread_mutex_lock(&lock);
    smooth_camera = !!enabled;
    pthread_mutex_unlock(&lock);
}

/* Retain motion below one tile; camera units stay the same at either zoom. */
static void take_pan(int factor, int *x, int *y)
{
    int sx = smooth_camera ? 1 : 32 / factor;
    int sy = smooth_camera ? 1 : 16 / factor;
    *x = (int)(pan_x / sx) * sx;
    *y = (int)(pan_y / sy) * sy;
    pan_x -= *x; pan_y -= *y;
}
static _Atomic unsigned frames;
static int button = -1, releasing, worker, target_x, target_y, target_pending, cancelling_aim, cancel_ack;
static unsigned press_frame, release_frame, cancel_frame;
static uint64_t release_ns;
static pthread_cond_t button_cond = PTHREAD_COND_INITIALIZER;

/* A surface point as a frame pixel: through the gesture's interface layer,
 * else the plain aspect fit. The outermost pixels are avoided: the cursor
 * resting there edge-scrolls (459250). */
static void frame_point(float nx, float ny, int *x, int *y)
{
    int w, h;
    void_input_screen_size(&w, &h);
    double fx, fy;
    surface_frame(nx, ny, w, h, &fx, &fy);
    if (touch_layer) part_source(&touch_part, fx, fy, touch_scale, &fx, &fy);
    *x = (int)fmax(1, fmin(w - 2, fx));
    *y = (int)fmax(1, fmin(h - 2, fy));
}
static float frame_scale(void)
{
    int w, h;
    void_input_screen_size(&w, &h);
    if (surface_width <= 0 || surface_height <= 0) return 1;
    return fminf((float)surface_width / (float)w, (float)surface_height / (float)h);
}
/* Beyond the Android touch slop (8 dp): about 3% of the surface height on phones. */
static int far_from_start(float nx, float ny)
{
    float sw = surface_width > 0 ? (float)surface_width : 1, sh = surface_height > 0 ? (float)surface_height : 1;
    return hypotf((nx - start_nx) * sw, (ny - start_ny) * sh) > .03f * sh;
}
/* The button helpers below run with `lock` held. */
static void release_now(void)
{
    if (target_pending) { void_input_mouse_warp(target_x, target_y); target_pending = 0; }
    if (button >= 0) void_input_mouse_button(button, 0);
    if (cancelling_aim == 1) void_input_mouse_button(1, 0);
    cancelling_aim = cancel_ack = 0;
    button = -1; releasing = 0;
}
static int sampled(unsigned since) { return frames - since >= 2; }
static void cancel_aim(void)
{
    if (aim_build && button == 0 && !releasing && !cancelling_aim) {
        /* 42D630 cancels BUILD on right-down before430830 sees left-up. */
        target_pending = 0; void_input_mouse_button(1, 1);
        cancelling_aim = 1; cancel_ack = 0; cancel_frame = frames; release_ns = void_monotonic_ns();
        pthread_cond_signal(&button_cond);
    }
    aim_build = 0; stronghold_display_magnifier(0, 0, 0);
}
static int cancel_sampled(void)
{
    if (!cancelling_aim) return 0;
    if (cancel_ack && sampled(cancel_frame) && void_monotonic_ns() - release_ns >= 50000000u) release_now();
    return 1;
}
static void *button_worker(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&lock);
    for (;;) {
        while (button < 0 || (!releasing && !target_pending && !cancelling_aim)) pthread_cond_wait(&button_cond, &lock);
        if (!cancel_sampled()) {
            if (target_pending && sampled(press_frame)) { void_input_mouse_warp(target_x, target_y); target_pending = 0; }
            uint64_t held_ns = void_monotonic_ns() - release_ns;
            if (releasing && ((sampled(release_frame) && held_ns >= 50000000u) || held_ns >= 400000000u)) { release_now(); continue; }
        }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 8000000;
        if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; ++until.tv_sec; }
        pthread_cond_timedwait(&button_cond, &lock, &until);
    }
    return NULL;
}
static void press(int which, int x, int y)
{
    release_now();
    void_input_mouse_warp(x, y);
    void_input_mouse_button(which, 1);
    button = which; press_frame = frames;
    if (!worker) {
        pthread_t thread;
        worker = pthread_create(&thread, NULL, button_worker, NULL) == 0 ? 1 : -1;
        if (worker > 0) pthread_detach(thread);
    }
}
/* A held button follows the finger once its press was sampled. */
static void move_to(int x, int y)
{
    if (button >= 0 && !sampled(press_frame)) { target_x = x; target_y = y; target_pending = 1; pthread_cond_signal(&button_cond); }
    else { target_pending = 0; void_input_mouse_warp(x, y); }
}
static void release(void)
{
    if (button < 0 || releasing) return;
    if (worker < 0) { release_now(); return; }   /* no helper thread: an instant release */
    releasing = 1; release_frame = frames; release_ns = void_monotonic_ns();
    pthread_cond_signal(&button_cond);
}
static void click(int which, int x, int y) { press(which, x, y); release(); }
/* A new touch: which interface layer (if any) is under it decides its mapping. */
static int original_hud_kind(int x, int y);
static void touch_begins(float nx, float ny)
{
    int w, h;
    void_input_screen_size(&w, &h);
    double fx, fy;
    surface_frame(nx, ny, w, h, &fx, &fy);
    pthread_mutex_lock(&display_lock);
    stronghold_part hit;
    touch_layer = frame_ready && published_width == w && published_height == h && pick_part(fx, fy, &hit) >= 0;
    if (frame_ready && published_width == w && published_height == h && published_used[0]) {
        stronghold_part hud = {0, 0, h, w * .5f, h};
        double sx, sy;
        part_source(&hud, fx, fy, published_scale, &sx, &sy);
        /* The active radar owns its rectangle, including transparent map pixels. */
        if (sx >= 0 && sx < w && sy >= 0 && sy < h && original_hud_kind((int)sx, (int)sy) == SH_TOUCH_RADAR) {
            hit = hud; touch_layer = 1;
        }
    }
    if (touch_layer) { touch_part = hit; touch_scale = published_scale; }
    pthread_mutex_unlock(&display_lock);
}
static int world_at(int y)
{
    int w, h;
    void_input_screen_size(&w, &h);
    return !touch_layer && in_game() && MEM32(DIALOG) == UINT32_MAX && y < h - HUD_HEIGHT;
}
static int original_world_kind(void)
{
    return void_vm_readable(0x1503B24u, 4) && MEM32(0x1503B24u) ? SH_TOUCH_BUILDING : SH_TOUCH_WORLD;
}
static int original_aim_valid(void)
{
    return aim_build && current_kind == SH_TOUCH_BUILDING && in_game() &&
        MEM32(DIALOG) == UINT32_MAX && void_vm_readable(0x1503B24u, 4) && MEM32(0x1503B24u) == aim_build;
}
static int original_area_build(uint32_t type)
{
    /* Original430830 previews while held; demolition431020 deletes on down. */
    return type == 25 || type == 46 || type == 27 || type == 26;
}
/* Guest-thread acknowledgement: a dialog may have swallowed right-down.
 * Keep left held until the original cancels; retry the right edge on return. */
static void original_cancel_frame(void)
{
    if (!cancelling_aim) return;
    /* Outside a match the construction callbacks are no longer dispatched. */
    if (!in_game()) { release_now(); return; }
    if (!void_vm_readable(0x1503B24u, 4)) return;
    if (!MEM32(0x1503B24u)) cancel_ack = 1;
    else if (sampled(cancel_frame) && in_game() && MEM32(DIALOG) == UINT32_MAX &&
             !MEM32(0xF67700u) && !MEM32(0xF67814u)) {
        int down = cancelling_aim == 2;
        void_input_mouse_button(1, down); cancelling_aim = down ? 1 : 2; cancel_frame = frames;
    }
    if (worker < 0) cancel_sampled();
}

/* SHA8b581f48's common HUD records, after the original constructor4BBD80
 * resolves inherited callbacks and sprite dimensions. 4BDBE0 stops this tree
 * at substates60/62. Match the active owner and the original hidden/disabled
 * words, rather than treating the book/radar artwork's rectangle as a button. */
static int original_hud_kind(int x,int y)
{
    if(in_game()&&MEM32(DIALOG)==5)return SH_TOUCH_MENU;
    if(!in_game()||MEM32(DIALOG)!=UINT32_MAX)return SH_TOUCH_UI;
    uint32_t panel=MEM32(STATE+4);
    if(MEM32(STATE)!=16&&(panel==60||panel==62))return SH_TOUCH_UI;
    /* The book's rectangle overlaps the radar's right edge by eleven pixels. */
    const uint32_t nodes[]={0x5A1240u,0x5A6A60u,0x5B1370u,0x5A16F0u,0x5A6B00u};
    for(unsigned i=0;i<sizeof nodes/sizeof *nodes;++i){
        int book = i >= 3;
        uint32_t node=nodes[i],owner=MEM32(node+76);
        if(!owner||owner!=MEM32(0xABB974u)||!void_vm_readable(owner,60)||MEM32(owner+24)||
            MEM16(node+54)||MEM16(node+56)||
            MEM32(node+20)!=(book?0x43ADA0u:0x42D590u)||(book&&MEM32(node+24)!=71))continue;
        int64_t left=(int32_t)MEM32(node+4)+(int64_t)(int32_t)MEM32(owner+4)-(int32_t)MEM32(owner+20);
        int64_t top=(int32_t)MEM32(node+8)+(int64_t)(int32_t)MEM32(owner+8);
        int w=(int32_t)MEM32(node+12),h=(int32_t)MEM32(node+16);
        if(w>0&&h>0&&x>=left&&x<left+w&&y>=top&&y<top+h)return book?SH_TOUCH_BOOK:SH_TOUCH_RADAR;
    }
    return stronghold_original_build_panel(x,y)?SH_TOUCH_BUILD_PANEL:SH_TOUCH_UI;
}
static void original_hud_action_locked(int action)
{
    /* A dialog or a panel slide may have appeared during the held gesture. */
    if(original_hud_kind(start_x,start_y)!=current_kind)return;
    if(action==SH_TOUCH_OPEN_TOOLS&&current_kind==SH_TOUCH_BOOK)ui_events|=4;
    else if(action==SH_TOUCH_RADAR_TOGGLE&&current_kind==SH_TOUCH_RADAR)command_locked('R',2);
    else if(action==SH_TOUCH_HOLD&&current_kind==SH_TOUCH_RADAR)command_locked(0x20,0);
}

/* The mouse path is always the game's own here: Java waits for this before
 * sending held buttons, as with Crusader's desktop mode. */
int stronghold_touch_desktop(void) { return !touch_direct; }

void stronghold_touch_enqueue(int action, unsigned sequence, float x, float y, float dx, float dy, float scale)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(dx) || !isfinite(dy) || !isfinite(scale)) return;
    if (action == SH_TOUCH_MODE) { stronghold_touch_abort(); touch_direct = scale > .5f; return; }
    if (action == SH_TOUCH_UI_SCALE) { stronghold_display_set_ui_scale(scale); return; }
    /* A mouse wheel keeps the game's own wheel (dx carries WHEEL_DELTA units);
     * the middle-button map drag needs the Crusader camera and is ignored. */
    if (action == SH_TOUCH_WHEEL) { if (dx) void_input_mouse_wheel((int)dx); return; }
    if (action == SH_TOUCH_GRAB) return;
    if (!touch_direct || action == SH_TOUCH_ZOOM_RESET) return;
    pthread_mutex_lock(&lock);
    if (cancelling_aim) {
        if (action == SH_TOUCH_QUERY) { current_sequence = sequence; current_kind = SH_TOUCH_BLOCKED; }
        pthread_mutex_unlock(&lock); return;
    }
    if (action == SH_TOUCH_QUERY) touch_begins(x, y);
    int px, py;
    frame_point(x, y, &px, &py);
    if (action != SH_TOUCH_QUERY && sequence != current_sequence) { pthread_mutex_unlock(&lock); return; }
    int free_button = button < 0 || releasing, dragging = button == 0 && !releasing;
    switch (action) {
    case SH_TOUCH_QUERY:
        cancel_aim();
        if (cancelling_aim) { current_sequence = sequence; current_kind = SH_TOUCH_BLOCKED; break; }
        release();
        aim_build = 0;
        stronghold_display_magnifier(0, 0, 0);
        /* Map touches outside a dialog and above the bottom panel act on the world. */
        current_kind = world_at(py) ? original_world_kind() : original_hud_kind(px,py);
        current_sequence = sequence; held = 0; pinch = 1;
        start_x = px; start_y = py; start_nx = x; start_ny = y;
        if (button < 0) void_input_mouse_warp(px, py);
        break;
    case SH_TOUCH_TAP: case SH_TOUCH_DOUBLE_TAP: case SH_TOUCH_UI_CLICK:
        if(current_kind==SH_TOUCH_MENU&&MEM32(DIALOG)==5&&void_vm_readable(0x1503CB8u,48)) {
            int left=(int32_t)MEM32(0x1503CC0u),top=(int32_t)MEM32(0x1503CC4u);
            int width=(int32_t)MEM32(0x1503CC8u),height=(int32_t)MEM32(0x1503CCCu);
            if(width>0&&height>0&&(px<left||py<top||px>=left+width||py>=top+height)) {
                release();tap_key(0x1b,0);held=1;break;
            }
        }
        click(0, px, py); break;
    case SH_TOUCH_HOLD:
        if(current_kind==SH_TOUCH_RADAR){original_hud_action_locked(action);held=1;break;}
        if (free_button) { click(1, start_x, start_y); held = 1; }
        break;
    case SH_TOUCH_OPEN_TOOLS:case SH_TOUCH_RADAR_TOGGLE:
        original_hud_action_locked(action);
        release();held=1;break;
    case SH_TOUCH_UI_HOLD_BEGIN:
        if (free_button) { click(1, start_x, start_y); held = 1; }
        break;
    case SH_TOUCH_CANCEL:
        cancel_aim();
        if (!cancelling_aim) click(1, px, py);
        held = 1; break;
    case SH_TOUCH_AIM_BEGIN:
        if (current_kind == SH_TOUCH_BUILDING && world_at(py)) {
            aim_build = MEM32(0x1503B24u);
            if (original_area_build(aim_build)) press(0, start_x, start_y);
            else if (button < 0) void_input_mouse_warp(px, py);
        }
        break;
    case SH_TOUCH_AIM_UPDATE:
        if (original_aim_valid()) {
            if (button == 0 && !releasing) move_to(px, py);
            else if (button < 0) void_input_mouse_warp(px, py);
        } else cancel_aim();
        break;
    case SH_TOUCH_AIM_COMMIT:
        if (original_aim_valid()) {
            if (original_area_build(aim_build)) { move_to(px, py); release(); }
            else click(0, px, py);
        } else cancel_aim();
        aim_build = 0; stronghold_display_magnifier(0, 0, 0); held = 1;
        break;
    case SH_TOUCH_BOX_BEGIN:
        press(0, start_x, start_y); break;
    case SH_TOUCH_BOX_UPDATE:
        if (dragging) move_to(px, py);
        break;
    case SH_TOUCH_UI_HOVER:
        /* A UI drag holds the left button from its start, as a mouse drag does. */
        if (current_kind!=SH_TOUCH_BUILD_PANEL&&current_kind!=SH_TOUCH_MENU&&free_button && !held && far_from_start(x, y)) { press(0, start_x, start_y); dragging = 1; }
        if (dragging) move_to(px, py);
        else if (button < 0) void_input_mouse_warp(px, py);
        break;
    case SH_TOUCH_UI_HOLD_END:
        if(current_kind==SH_TOUCH_RADAR){release();held=1;break;}
        /* Android ended a moved touch: below the drag distance it is still a tap. */
        if (dragging) { move_to(px, py); release(); }
        else if (!held) click(0, current_kind == SH_TOUCH_MENU ? px : start_x,
                                  current_kind == SH_TOUCH_MENU ? py : start_y);
        break;
    case SH_TOUCH_BOX_END:
        if (dragging) { move_to(px, py); release(); }
        break;
    case SH_TOUCH_BUILD_SWIPE:
        if(current_kind==SH_TOUCH_BUILD_PANEL&&dx)stronghold_original_build_swipe(dx<0?1:-1);
        release();held=1;break;
    case SH_TOUCH_PAN: case SH_TOUCH_TRANSFORM: {
        if (!in_game() || MEM32(DIALOG) != UINT32_MAX ||
            !((current_kind >= SH_TOUCH_WORLD && current_kind <= SH_TOUCH_BUILDING) ||
              (action == SH_TOUCH_PAN && current_kind == SH_TOUCH_RADAR &&
               original_hud_kind(start_x,start_y) == SH_TOUCH_RADAR))) break;
        float s = frame_scale() / (current_kind == SH_TOUCH_RADAR ? 2.f : 1.f);
        int w, h;
        void_input_screen_size(&w, &h);
        /* Surface-normalized deltas as frame pixels; the map follows the finger. */
        pan_x += dx * (float)(surface_width > 0 ? surface_width : w) / s;
        pan_y += dy * (float)(surface_height > 0 ? surface_height : h) / s;
        if (action == SH_TOUCH_TRANSFORM && scale > 0) {
            pinch *= scale;
            int zoomed_out = void_vm_readable(CAMERA + 0x8C, 4) && MEM32(CAMERA + 0x8C);
            if ((pinch > 1.3f && zoomed_out) || (pinch < .77f && !zoomed_out)) { tap_key('Z', 0); pinch = 1; }
        }
        break;
    }
    case SH_TOUCH_ROTATE:
        tap_key(dx > 0 ? 'C' : 'X', 0); break;
    case SH_TOUCH_ABORT:
        cancel_aim(); if (!cancelling_aim) release(); held = 1; break;
    default:
        break;
    }
    pthread_mutex_unlock(&lock);
}

int stronghold_touch_status(unsigned sequence)
{
    pthread_mutex_lock(&lock);
    int kind = sequence == current_sequence ? current_kind : SH_TOUCH_BLOCKED;
    pthread_mutex_unlock(&lock);
    return kind;
}

/* Android onPause: no guest memory or calls, only input release. */
void stronghold_touch_abort(void)
{
    pthread_mutex_lock(&lock);
    cancel_aim(); if (!cancelling_aim) release_now();
    pan_x = pan_y = 0; current_sequence = 0; touch_layer = 0;
    stronghold_display_magnifier(0, 0, 0);
    pthread_mutex_unlock(&lock);
}

/* tools/original_hooks.py: once per frame in WinMain's loop (532EBA), on the
 * game thread, before the original scroll update. */
void stronghold_original_frame(void)
{
    ++frames;
    stronghold_game_ui_frame();
    stronghold_original_build_frame(touch_direct);
    original_commands();
    pthread_mutex_lock(&lock);
    original_cancel_frame();
    if (button >= 0) pthread_cond_signal(&button_cond);
    int factor = MEM32(CAMERA + 0x8C) ? 2 : 1;
    int x, y;
    take_pan(factor, &x, &y);
    pthread_mutex_unlock(&lock);
    if ((!x && !y) || !in_game() || MEM32(DIALOG) != UINT32_MAX) return;
    /* Camera units are full-zoom pixels; zoomed out, a frame pixel covers two. */
    MEM32(CAMERA + 0x74) -= (uint32_t)(x * factor);
    MEM32(CAMERA + 0x78) -= (uint32_t)(y * factor);
    void_regs saved;
    void_regs_save(&saved);
    void_call_guest_this(CAMERA_CLAMP, CAMERA, 0, NULL);
    void_regs_load(&saved);
}
