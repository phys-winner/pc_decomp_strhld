/* Crisp GDI text: recorded on the guest thread by the GDI text path, hidden
 * from the layer it landed in and drawn again by the presenter at output
 * resolution, right after that layer (groups as in stronghold_display_layer_view). */
#ifndef STRONGHOLD_TEXT_OVERLAY_H
#define STRONGHOLD_TEXT_OVERLAY_H
#include <stdint.h>
#include "display.h"

/* gdi_android.c, around the blend of one run into 16-bit surface memory: the
 * drawable (already clipped) area is w x h at dst, the run's coverage mask
 * (mw x mh, stride bytes per row) goes to x,y in it. begin returns 1 when the
 * run is being noted; end follows the blend. The raster output is unchanged. */
int stronghold_text_overlay_begin(uint32_t dst, int w, int h, int pitch, int x, int y,
    const uint8_t *mask, int mw, int mh, int stride, const uint16_t *text, int count,
    int size, int weight, int italic, uint32_t color, uint32_t background, int opaque);
void stronghold_text_overlay_end(void);

/* Presenter, guest thread (the original present call), once per newly
 * published composed frame before its CPU layers are composed or uploaded:
 * layers[g] is UI group g (RGB565, `stride` pixels per row, NULL if unused),
 * coverage[g] its 0-255 coverage (same stride; NULL keeps the raster copy of
 * runs whose stores raised coverage, e.g. text over the world), valid in
 * bounds[g] = left,top,right,bottom. Every noted run whose raster rows are
 * still intact there gets its text-free pixels and coverage back and is
 * queued for stronghold_text_overlay_draw(g). hidden[g] (optional) receives
 * the union of the edited rectangles: a presenter uploading only changed rows
 * adds it to that layer's dirty rectangle. Returns runs hidden. */
int stronghold_text_overlay_hide_layers(uint16_t *const layers[SH_UI_LAYERS], uint8_t *const coverage[SH_UI_LAYERS],
                                        const int bounds[SH_UI_LAYERS][4], int stride, int width, int height, int hidden[SH_UI_LAYERS][4]);
/* The same for an uncomposed frame (group -1, menus): the frame as presented,
 * edited in place; restore puts the raster copies back after the upload when
 * the pixels are guest memory. Call for every presented uncomposed frame. */
int stronghold_text_overlay_hide_frame(uint16_t *pixels, int stride, int width, int height);
void stronghold_text_overlay_restore_frame(void);
/* Old presenter (ddraw_present.c) only: hide_layers on display.c's published
 * layers when a new composed frame is pending, a no-op otherwise. Its single
 * composed texture puts every group's text above all layers, so runs under a
 * higher group's bounding box keep their raster copy there. A presenter that
 * draws text after each layer calls hide_layers itself on the layers it is
 * about to upload (those of a fresh stronghold_display_frame). */
void stronghold_text_overlay_publish(void);

/* GL thread, inside the presenter's frame, with the viewport set to the whole
 * window: draws the runs queued for group (0..SH_UI_LAYERS-1, or -1 for an uncomposed frame).
 * Uses its own program, texture unit 0, blending, GL_ARRAY_BUFFER and vertex
 * attributes 0/1; leaves blending off and the buffer binding at 0. */
void stronghold_text_overlay_draw(int group, int viewport_width, int viewport_height);
/* The GL context was lost or replaced: forget GL objects and the atlas. */
void stronghold_text_overlay_reset_gl(void);

/* Implemented in display.c next to the layer helpers (no display.h change). */
int stronghold_display_crisp_text(void);
/* Coverage recorded so far in the open UI pass for row y of its current group
 * (the target of the store being made), NULL outside a pass. */
const uint8_t *stronghold_display_capture_row(int *group, int y, int *width);
/* display.c's CPU layers (ui[g].pixels and .coverage, stride = width) of a
 * newly published, not yet composed frame and their valid bounds; 0 when none. */
int stronghold_display_text_layers(uint16_t *layers[SH_UI_LAYERS], uint8_t *coverage[SH_UI_LAYERS], int bounds[SH_UI_LAYERS][4], int *width, int *height);
/* gdi_android.c: TextRaster.render into a tight width x height coverage image. */
int void_android_text_raster(const uint16_t *text, int count, int size, int weight, int italic,
    float scale, float dx, float dy, int width, int height, uint8_t *out);

#endif
