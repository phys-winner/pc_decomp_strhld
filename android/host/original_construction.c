/* Stronghold.exe 8b581f48: its own construction callbacks and widget layout.
 * Run guest reads/calls only on the guest thread; Android queues the swipe. */
#ifdef STRONGHOLD_ORIGINAL
#include "host.h"
#include "display.h"
#include "original_construction.h"
#include <stdatomic.h>

#define BUILD 0x1503B24u
#define STATE 0x1503D38u
#define DIALOG 0x1503CE4u
#define OWNER 0xABB970u
#define SELECT 0x43A770u
static _Atomic int swipe;
static int enabled, help, redraw, click_pending;
static uint32_t last_node, last_type, previous;
static const int tabs[]={10,20,40,30,28,25,26,27,29};
static int parent(int section)
{
    switch(section){case 11:case 12:case 13:case 14:case 18:case 19:return 10;
    case 15:case 33:return 30;default:return section;}
}
static int active(void)
{
    return void_vm_readable(STATE,8)&&void_vm_readable(DIALOG,4)&&
           MEM32(STATE)==14&&MEM32(DIALOG)==UINT32_MAX;
}
int stronghold_original_build_panel(int x,int y)
{
    if(!active()||!void_vm_readable(0xABB980u,8))return 0;
    int section=parent((int)MEM32(STATE+4));
    int ox=(int32_t)MEM32(0xABB980u),oy=(int32_t)MEM32(0xABB984u);
    return ((section>=10&&section<=14)||section==20||(section>=25&&section<=30)||section==40)&&
           (int64_t)x>=ox&&(int64_t)x<ox+510LL&&(int64_t)y>=oy+472LL&&(int64_t)y<oy+600LL;
}
void stronghold_original_build_swipe(int step) { atomic_store(&swipe,step>0?1:step<0?-1:0); }
static uint32_t call(uint32_t fn,uint32_t self,int argc,uint32_t *args)
{
    void_regs saved;void_regs_save(&saved);g_ecx=self;
    uint32_t result=void_call_guest(fn,argc,args,0);void_regs_load(&saved);return result;
}
void stronghold_original_build_frame(int native_enabled)
{
    enabled=native_enabled;help=0;
    stronghold_display_highlight(0,0,0,0);
    uint32_t building=void_vm_readable(BUILD,4)?MEM32(BUILD):0;
    if(click_pending){last_type=building;click_pending=0;}
    if(building!=previous){previous=building;redraw=1;}
    int step=atomic_exchange(&swipe,0);
    if(!enabled||!active()||!step)return;
    int current=(int)MEM32(STATE+4),section=parent(current),index=-1;
    for(int i=0;i<9;++i)if(tabs[i]==section)index=i;
    if(index<0)return;
    uint32_t mode=MEM32(0x1503D84u);
    int editor=mode==1||mode==6;
    /* 456070 uses exactly this keep+granary gate for the ordinary tabs. */
    int economy=(int32_t)call(0x44BFD0u,0xBCAFB8u,0,NULL)>0;
    int order=section!=current?1:step;
    for(int n=1;n<9;++n){
        int next=tabs[(index+order*n+81)%9];
        if((next==26||next==27||next==29)?!editor:(!editor&&!economy))continue;
        if(current==61||(!editor&&current==48))return;
        /* 4BC8B0 negates +44; 4BC900 then slides the original panel. */
        MEM32(OWNER+44)=step>0?UINT32_MAX:1u;
        uint32_t arg=(uint32_t)next;call(0x43ADA0u,0,1,&arg);return;
    }
}
void stronghold_original_build_click(uint32_t node)
{
    if(void_vm_readable(node,80)&&MEM32(node+20)==SELECT){last_node=node;click_pending=1;}
}
int stronghold_original_build_redraw(void)
{
    int changed=redraw;redraw=0;
    return enabled&&active()&&(changed||MEM32(BUILD));
}
void stronghold_original_build_draw(uint32_t node)
{
    if(!enabled||!active()||!MEM32(BUILD)||MEM32(OWNER+24)||!void_vm_readable(node,80))return;
    uint32_t building=MEM32(BUILD),owner=MEM32(node+76);
    if(MEM32(node+20)!=SELECT||MEM16(node+54)||MEM16(node+56)||
       (MEM32(node+24)!=building&&(node!=last_node||last_type!=building))||
       !void_vm_writable(owner,60)||(int32_t)MEM32(node+12)<=0||(int32_t)MEM32(node+16)<=0)return;
    /* Original 4BC600 uses +50 for the hover frame; 4BDDF0 reads owner+56
     * and dispatches the original localized building name and resource cost. */
    MEM16(node+50)=1;MEM32(owner+56)=node;help=1;
    stronghold_display_highlight((int32_t)MEM32(node+4)+(int32_t)MEM32(owner+4),
        (int32_t)MEM32(node+8)+(int32_t)MEM32(owner+8),MEM32(node+12),MEM32(node+16));
}
uint32_t stronghold_original_build_help(uint32_t original) { return enabled&&active()&&help?1:original; }
#endif
