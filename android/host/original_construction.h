#ifndef STRONGHOLD_ORIGINAL_CONSTRUCTION_H
#define STRONGHOLD_ORIGINAL_CONSTRUCTION_H
#include <stdint.h>
int stronghold_original_build_panel(int x, int y);
void stronghold_original_build_swipe(int step);
void stronghold_original_build_frame(int enabled);
void stronghold_original_build_click(uint32_t node);
void stronghold_original_build_draw(uint32_t node);
int stronghold_original_build_redraw(void);
uint32_t stronghold_original_build_help(uint32_t original);
#endif
