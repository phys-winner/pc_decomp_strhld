#ifndef STRONGHOLD_GAME_TEXT_H
#define STRONGHOLD_GAME_TEXT_H
#include <stdint.h>
/* Called on the guest thread; returns the Android editor event bit. */
int stronghold_text_frame(uint32_t state, uint32_t modal);
void stronghold_text_focus(uint32_t editor);
#endif
