#ifndef STRONGHOLD_TOUCH_H
#define STRONGHOLD_TOUCH_H
#include <stdint.h>
#include "touch_api.h"

/* Generated engine seams. All run with the guest machine owned. They consume
 * semantic actions, never fabricate DirectInput or Win32 battlefield buttons. */
int stronghold_touch_callback(int kind);
uint32_t stronghold_touch_legacy(uint32_t original);
uint32_t stronghold_touch_x(uint32_t original);
uint32_t stronghold_touch_y(uint32_t original);
uint32_t stronghold_touch_ui_x(uint32_t original);
uint32_t stronghold_touch_ui_y(uint32_t original);
uint32_t stronghold_touch_ui_button(uint32_t original);
int stronghold_touch_ui_redraw(void);
uint32_t stronghold_touch_ui_help(uint32_t original);
int stronghold_touch_selection(void);
int stronghold_touch_group_unfiltered(void);
int stronghold_touch_lord_control(void);
int stronghold_touch_order(int context);
int stronghold_touch_ability(void);
int stronghold_touch_cancel(void);
int stronghold_touch_inspect(void);
int stronghold_touch_picker(void);
uint32_t stronghold_touch_placement(int predicate, uint32_t original);
int stronghold_touch_known(void);
int stronghold_touch_selftest(void);

#endif
