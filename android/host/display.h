#ifndef STRONGHOLD_DISPLAY_H
#define STRONGHOLD_DISPLAY_H
#include <stdint.h>
#include <math.h>

/* A top-left notice starts beside the cutout, also when the game is letterboxed. */
static inline int stronghold_notice_margin(int w, int h, int sw, int sh, float left, float scale)
{
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return 16;
    double fit = fmin((double)sw/w, (double)sh/h);
    double inset = fmax(0, (left*sw - (sw-w*fit)*.5)/fit);
    return (int)ceil(inset/fmin(1,scale)) + 16;
}

/* Chosen before the guest thread starts; a fresh process applies changes. */
void stronghold_display_configure(int width, int height);
int stronghold_display_requested(int *width, int *height);
int stronghold_display_dimensions(uint32_t screen);
uint32_t stronghold_display_width(uint32_t original);
uint32_t stronghold_display_height(uint32_t original);
uint32_t stronghold_display_half_width(void);
void stronghold_display_viewport(uint32_t map);
void stronghold_display_clear(uint32_t stack);
uint32_t stronghold_display_edge_name(uint32_t name);
int stronghold_display_hud_edge(uint32_t renderer, uint32_t stack, int right);
void stronghold_display_hud_clip(uint32_t renderer, uint32_t saved[4], int restore);

/* Touch points/deltas use the Android surface's normalized coordinates. */
void stronghold_display_smooth_camera(int enabled);
void stronghold_display_set_ui_scale(float scale);
void stronghold_display_powers_scale(float scale);
void stronghold_display_cutout(float left);
void stronghold_display_set_render_scale(float scale);
/* Total zoom of the last completed world frame; independent of HUD/quality. */
float stronghold_display_world_zoom(void);
void stronghold_display_transform(float nx, float ny, float ndx, float ndy, float factor);
/* A new touch sequence re-arms the 100% detent; it holds once per gesture. */
void stronghold_display_zoom_session(void);
/* Returns to the original 1:1 grid around the centre of the view. */
void stronghold_display_zoom_reset(void);
/* Bottom-HUD rectangle (layer coordinates) shown brighter; width 0 clears. */
void stronghold_display_highlight(int left, int top, int width, int height);
/* Legacy single filter (0 bilinear, 1 sharp pixels, 2 bicubic) for both the
 * world and the UI, sharpness 0..1; the getter is the OpenGL ES 2 fallback's. */
void stronghold_display_set_image_filter(int mode, float sharpness);
int stronghold_display_image_filter(float *sharpness);
/* Per-layer presentation (0.9): SH_FILTER_* for the world and for the UI layers,
 * sharpness 0..1, deband smooths RGB565 gradient steps of the world, crisp_text
 * redraws recorded GDI text at output resolution. Any thread; applies next frame. */
enum { SH_FILTER_BILINEAR = 0, SH_FILTER_PIXELS = 1, SH_FILTER_BICUBIC = 2, SH_FILTER_FSR = 3 };
void stronghold_display_set_filters(int world, int ui, float sharpness, int deband, int crisp_text);
/* Defaults until set: FSR for the world and the UI, sharpness 0.25, no deband. */
void stronghold_display_filters(int *world, int *ui, float *sharpness, int *deband, int *crisp_text);
/* Placement loupe at a finger, normalized surface coordinates; active 0 hides it. */
void stronghold_display_magnifier(int active, float nx, float ny);
/* Returns 1 with the finger while the loupe is shown. */
int stronghold_display_loupe(float *nx, float *ny);
void stronghold_present_loupe_settings(int size, int zoom);
/* Loupe geometry in window pixels for a finger at (fx, fy): up-left of it,
 * up-right near the left edge, beside it near the top, always inside. */
static inline void stronghold_loupe_place(float fx, float fy, int ww, int wh, float scale,
                                          float *cx, float *cy, float *radius)
{
    float s = (float)(ww < wh ? ww : wh), r = s * .13f * scale, m = s * .02f;
    if (r < 40) r = 40;
    float x = fx - r * .85f, y = fy - r * 1.3f;
    if (x - r < m) x = fx + r * .85f;
    if (y - r < m) { y = fy; x = fx - 2.6f * r >= m ? fx - 1.6f * r : fx + 1.6f * r; }
    if (x > ww - r - m) x = ww - r - m;
    if (x < r + m) x = r + m;
    if (y > wh - r - m) y = wh - r - m;
    if (y < r + m) y = r + m;
    *cx = x; *cy = y; *radius = r;
}
/* Removes the colour-key background from a partially covered UI pixel:
 * out = pixel + (below - clear) * (1 - alpha), per RGB565 channel. This is
 * premultiplied "over" with premul = pixel - clear * (1 - alpha), which is
 * what the GPU presenter uploads; an opaque pixel equal to clear stays. */
static inline uint16_t stronghold_composite_pixel(uint16_t pixel, uint16_t below, unsigned alpha, uint16_t clear)
{
    if (alpha == 255) return pixel;
    if (!alpha) return below;
    unsigned result = 0;
    static const unsigned shifts[3] = {0, 5, 11}, limits[3] = {31, 63, 31};
    for (int i = 0; i < 3; i++) {
        int p = (pixel >> shifts[i]) & limits[i], b = (below >> shifts[i]) & limits[i], k = (clear >> shifts[i]) & limits[i];
        int n = (p * 255 + (b - k) * (int)(255 - alpha) + 127) / 255;
        if (n < 0) n = 0;
        if (n > (int)limits[i]) n = (int)limits[i];
        result |= (unsigned)n << shifts[i];
    }
    return (uint16_t)result;
}
/* One in-game frame for the GPU presenter: the world raster and the UI
 * layers with the transforms of stronghold_display_compose(). Layer rows
 * have a pitch of width; dirty rectangles collect changes since the last
 * call. Part p of the composition draws layer part[p].layer's rows
 * [top, bottom) scaled by ui_scale about its anchor, in order. */
typedef struct { int left, top, right, bottom; } stronghold_rect;
/* A zero part scale inherits ui_scale. */
typedef struct { int layer, top, bottom; float anchor_x, anchor_y, scale; } stronghold_part;
/* Edge-anchored original overlays stay apart from the movable dialogs. */
#ifdef STRONGHOLD_ORIGINAL
#define SH_UI_LAYERS 7
#define SH_UI_PARTS 8
#define SH_UI_TOP_ROWS 0
#else
#define SH_UI_LAYERS 8
#define SH_UI_PARTS 9
#define SH_UI_TOP_ROWS 128
#endif
/* Back-to-front: original edge notices stay below centered modal dialogs. */
#ifdef STRONGHOLD_ORIGINAL
static const uint8_t SH_UI_ORDER[SH_UI_LAYERS] = {0, 1, 4, 3, 5, 6, 2};
#else
static const uint8_t SH_UI_ORDER[SH_UI_LAYERS] = {0, 1, 3, 4, 5, 6, 7, 2};
#endif
typedef struct {
    int width, height;
    const uint16_t *world;
    int raster_width, raster_height;
    float world_scale_x, world_scale_y;   /* frame pixels per raster texel */
    float ui_scale;
    uint16_t key;
    int fresh;                             /* a new frame since the last call */
    int hud_opaque;                        /* group 0's bottom 128 rows fully covered */
    struct {
        const uint16_t *pixels;
        const uint8_t *coverage;
        int used;
        stronghold_rect bounds, dirty;
    } layer[SH_UI_LAYERS];
    stronghold_part part[SH_UI_PARTS];
    int parts;
    stronghold_rect highlight;             /* group 0 layer pixels, empty if none */
} stronghold_frame;
/* Presenter thread (the guest's): 1 fills *frame for a composited frame,
 * 0 means present the given pixels unchanged, -1 skip (UI pass open). */
int stronghold_display_frame(stronghold_frame *frame, int width, int height, int bpp);
/* Layer of a raster address written by the guest: 0..SH_UI_LAYERS-1 the captured UI groups
 * (layer pixel in *x,*y), SH_UI_LAYERS the persistent HUD surface (same pixel grid as
 * group 0's bottom rows), -1 anything else. Guest thread, during a frame. */
int stronghold_display_locate(uint32_t address, int *x, int *y);
/* Where a layer pixel lands in the GL viewport (pixels, origin top-left) and
 * the layer-to-view scale, for overlays drawn by the presenter. Group -1 is
 * the whole presented frame (menus, no composition). Returns 0 if unknown. */
int stronghold_display_layer_view(int group, float lx, float ly, float *vx, float *vy, float *scale);
int stronghold_display_map_point(float nx, float ny, int *gx, int *gy);
int stronghold_display_world_point(float nx, float ny, int *gx, int *gy);
int stronghold_display_ui_point(int group, float nx, float ny, int *gx, int *gy);
float stronghold_display_ui_dy(float normalized_dy);
float stronghold_display_ui_dx(float normalized_dx);
void stronghold_display_surface_size(int width, int height);

/* Guest-thread frame seams; the original viewport and clamp still run. */
void stronghold_display_frame_begin(void);
void stronghold_display_world_begin(uint32_t map);
void stronghold_display_world_render(uint32_t map);
uint32_t stronghold_display_world_width(void);
uint32_t stronghold_display_world_height(void);
void stronghold_display_ui_begin(uint32_t map);
void stronghold_display_ui_group(uint32_t root, int enter);
void stronghold_display_resource_overlay(uint32_t item, int before);
void stronghold_display_ui_end(void);
const uint8_t *stronghold_display_compose(const uint8_t *pixels, int width, int height,
                                         int *pitch, int bpp);
/* Original raster stores supply coverage; RGB565 values are never alpha. */
extern int stronghold_display_capture_active;
void stronghold_display_written(uint32_t address, unsigned bytes);
void stronghold_display_blended(uint32_t address, unsigned bytes, unsigned background);
void stronghold_display_clear_scope(int enter);
#endif
