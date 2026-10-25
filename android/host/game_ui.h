#ifndef STRONGHOLD_GAME_UI_H
#define STRONGHOLD_GAME_UI_H
#include <stdint.h>
enum { SH_GAME_SETTINGS = 9 };
enum { SH_UI_SETTINGS = 1, SH_UI_SETTINGS_GRAPHICS = 64, SH_UI_SETTINGS_AUDIO = 128 };
/* [0] bits: 1 in game, 2 editor, 4 LAN, 8 main menu; [1] groupable units;
 * [2]/[3] occupied group/bookmark slots; [4..7] HUD geometry; [8] view bits:
 * 1 radar force colours, 2 flat relief; [9] original game speed. */
enum { SH_CONTROL_STATE = 10 };
void stronghold_game_control_state(int *values);
void stronghold_game_command(int key, int modifiers);
void stronghold_game_tools(void);
void stronghold_game_ui_frame(void);
void stronghold_game_ui_click(uint32_t node);
void stronghold_game_ui_skip(int enabled);
int stronghold_game_ui_event(void);
int stronghold_game_settings_get(int *values);
void stronghold_game_setting_set(int index, int value);
int stronghold_game_editor_get(uint16_t *text, int *limit, int *bank);
int stronghold_game_editor_set(const uint16_t *text, int length, int token);
void stronghold_game_editor_request(void);
#endif
