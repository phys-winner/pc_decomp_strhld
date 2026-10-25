/* Each APK process loads one AOT library: the EXEs share guest addresses. */
#ifndef STRONGHOLD_EDITION_H
#define STRONGHOLD_EDITION_H

#ifdef STRONGHOLD_ORIGINAL
/* The original Stronghold HD: its own installation and profile. Its screen
 * object (00B182C8) is not Crusader's, so the Crusader mode-3 override in
 * ddraw.c/core.c stays off (page 0 is never committed); the engine's own
 * mode table is used instead (stronghold_original.c). */
#define GAME_INSTALL "C:\\Games\\Stronghold HD"
#define GAME_EXE "Stronghold.exe"
#define GAME_TITLE "Stronghold HD"
#define GAME_ENTRY 0x0053B527u
#define GAME_SCREEN 0x00000000u
#define GAME_MODE_CALC 0x00000000u
#define GAME_RESOURCE_DELTA 0u
#define GAME_ROOT_DEFAULT "/sdcard/Stronghold/stronghold-hd"
#define GAME_IMAGES_DEFAULT "/sdcard/Stronghold/lifted-stronghold"
#define GAME_DATA_DEFAULT "/sdcard/Stronghold/userdata-stronghold"
#define GAME_PROFILE "Stronghold"
#define GAME_SAVES "Saves"
/* files.c selftest: a root asset and its magic (the original ships no faces.bmp). */
#define GAME_PROBE_UPPER "HAND.ANI"
#define GAME_PROBE "hand.ani"
#define GAME_PROBE_MAGIC "RI"
#else
#define GAME_INSTALL "C:\\Games\\Stronghold Crusader Extreme HD"
#define GAME_ROOT_DEFAULT "/sdcard/Stronghold/game"
#define GAME_IMAGES_DEFAULT "/sdcard/Stronghold/lifted"
#define GAME_PROFILE "Stronghold Crusader"
#define GAME_PROBE_UPPER "FACES.BMP"
#define GAME_PROBE "faces.bmp"
#define GAME_PROBE_MAGIC "BM"
#ifdef STRONGHOLD_CLASSIC
#define GAME_EXE "Stronghold Crusader.exe"
#define GAME_TITLE "Stronghold Crusader HD"
#define GAME_ENTRY 0x005849F6u
#define GAME_SCREEN 0x00FD5780u
#define GAME_MODE_CALC 0x0046FE20u
#define GAME_RESOURCE_DELTA 0u
#define GAME_DATA_DEFAULT "/sdcard/Stronghold/userdata-classic"
#define GAME_SAVES "Saves"
#else
#define GAME_EXE "Stronghold_Crusader_Extreme.exe"
#define GAME_TITLE "Stronghold Crusader Extreme HD"
#define GAME_ENTRY 0x00584E46u
#define GAME_SCREEN 0x00FD5BE0u
#define GAME_MODE_CALC 0x00470030u
#define GAME_RESOURCE_DELTA 0x10u
#define GAME_DATA_DEFAULT "/sdcard/Stronghold/userdata"
#define GAME_SAVES "SavesExtreme"
#endif
#endif
#define GAME_COMMAND_LINE "\"" GAME_INSTALL "\\" GAME_EXE "\""

#endif
