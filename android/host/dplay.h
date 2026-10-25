#ifndef STRONGHOLD_DPLAY_H
#define STRONGHOLD_DPLAY_H
#include <stdint.h>
uint32_t stronghold_dplay_create(uint32_t clsid,uint32_t iid,uint32_t out);
int stronghold_dplay_active(void);
int stronghold_dplay_disconnected(void);
void stronghold_lan_address(char ip[64]);
#endif
