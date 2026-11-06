#ifndef STRONGHOLD_MODS_H
#define STRONGHOLD_MODS_H
#include <stdint.h>
extern unsigned char stronghold_ucp_enabled[128];
const char *stronghold_mods_configure(const char *path);
int stronghold_ucp_apply(void);
int stronghold_mod_count(void);
const char *stronghold_mod_root(int index);
#endif
