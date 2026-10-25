/* Stronghold.exe SHA8b581f48: alternate HUD colours use the original team
 * comparison (42EC26,46CC40,44C100) and original 555/565 palette entries. */
#ifdef STRONGHOLD_ORIGINAL
#include "host.h"
#include "original_radar.h"
#include <stdatomic.h>
static _Atomic int radar_team_colors;
int stronghold_original_radar_colors(void) { return atomic_load(&radar_team_colors); }
void stronghold_original_radar_toggle(void)
{
    atomic_fetch_xor(&radar_team_colors,1);
    /* Original 4959A0 tests this flag before redrawing its radar object. */
    if(void_vm_writable(0x1012178u,4))MEM32(0x1012178u)=1;
}
uint32_t stronghold_original_radar_color(uint32_t owner,uint32_t original)
{
    if(!stronghold_original_radar_colors()||owner<1||owner>8||
       !void_vm_readable(0x1008144u,4)||!void_vm_readable(0xBF6424u,36))return original;
    uint32_t player=MEM32(0x1008144u);
    if(player<1||player>8)return original;
    /* Only the palette index changes. Original neutral branches, ownership,
     * flashing phase and pixel-format selection still run unchanged. */
    return owner==player||MEM32(0xBF6424u+4*owner)==MEM32(0xBF6424u+4*player)?8u:2u;
}
#endif
