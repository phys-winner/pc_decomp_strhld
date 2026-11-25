#ifndef STRONGHOLD_RENDERER_H
#define STRONGHOLD_RENDERER_H

#include <stdint.h>

/* Screen entry points called by the three lifted engines. */
void stronghold_render_open(void);
void stronghold_render_close(void);
void stronghold_render_configure(void);
void stronghold_render_set_mode(void);
void stronghold_render_capabilities(void);
void stronghold_render_modes(void);
void stronghold_render_allocate(void);
void stronghold_render_release(void);
void stronghold_render_back_buffer(void);
void stronghold_render_describe(void);
void stronghold_render_format(void);
void stronghold_render_lock(void);
void stronghold_render_unlock(void);
void stronghold_render_restore(void);
void stronghold_render_copy(void);
void stronghold_render_copy_at(void);
void stronghold_render_swap(void);
void stronghold_render_acquire_dc(void);
void stronghold_render_release_dc(void);

uint32_t stronghold_renderer_redirect(uint32_t token, uint32_t pixels);
int stronghold_renderer_dc_pixels(uint32_t token, uint32_t *pixels, int *w, int *h, int *pitch, int *bpp);
void stronghold_renderer_present_pixels(const uint8_t *pixels, int w, int h, int pitch, int bpp);
int stronghold_renderer_selftest(void);

#endif
