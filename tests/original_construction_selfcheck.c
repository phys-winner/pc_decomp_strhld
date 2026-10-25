#include "../android/host/original_construction.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
uint32_t g_ecx;
static int economy,calls,next,stamp=1234;
static stronghold_rect highlight;
int void_vm_readable(uint32_t a,uint32_t n){return a>=0x400000&&(uint64_t)a+n<=0x4000000;}
int void_vm_writable(uint32_t a,uint32_t n){return void_vm_readable(a,n);}
void void_regs_save(void_regs *r){memset(r,0,sizeof *r);r->eax=stamp;}
void void_regs_load(const void_regs *r){stamp=r->eax;}
uint32_t void_call_guest(uint32_t fn,int argc,const uint32_t *args,int callee)
{
    assert(!callee);stamp=0;
    if(fn==0x44BFD0u){assert(g_ecx==0xBCAFB8u&&!argc);return economy;}
    assert(fn==0x43ADA0u&&argc==1);next=args[0];calls++;return 0;
}
void stronghold_display_highlight(int l,int t,int w,int h){highlight=(stronghold_rect){l,t,l+w,t+h};}
static void slide(int panel,int mode,int direction,int expected)
{
    MEM32(STATE+4)=panel;MEM32(0x1503D84u)=mode;next=0;
    stronghold_original_build_swipe(direction);stronghold_original_build_frame(1);
    assert(next==expected&&stamp==1234);
    if(expected)assert(MEM32(OWNER+44)==(direction>0?UINT32_MAX:1u));
}
int main(void)
{
    assert(mmap(GPTR(0x400000),0x3c00000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
    MEM32(STATE)=14;MEM32(DIALOG)=UINT32_MAX;MEM32(0xABB980u)=268;
    MEM32(STATE+4)=10;
    assert(stronghold_original_build_panel(300,500));
    assert(!stronghold_original_build_panel(300,400)&&!stronghold_original_build_panel(800,500));
    economy=1;slide(10,0,1,20);slide(10,0,-1,25);slide(11,0,-1,20);
    slide(25,0,1,10);slide(25,1,1,26);slide(29,6,1,10);
    economy=0;slide(10,0,1,0);slide(10,1,1,20);
    MEM32(DIALOG)=3;slide(10,1,1,0);assert(!stronghold_original_build_panel(300,500));
    MEM32(DIALOG)=UINT32_MAX;MEM32(STATE+4)=10;
    uint32_t node=0x600000,owner=0x601000;
    MEM32(BUILD)=13;MEM32(node+20)=SELECT;MEM32(node+24)=13;
    MEM32(node+76)=owner;MEM32(node+4)=30;MEM32(node+8)=480;
    MEM32(node+12)=40;MEM32(node+16)=30;MEM32(owner+4)=268;
    stronghold_original_build_frame(1);assert(stronghold_original_build_redraw());
    stronghold_original_build_draw(node);
    assert(MEM16(node+50)==1&&MEM32(owner+56)==node&&stronghold_original_build_help(0)==1);
    assert(highlight.left==298&&highlight.bottom==510);
    MEM32(BUILD)=0;stronghold_original_build_frame(1);
    assert(!stronghold_original_build_help(0)&&stronghold_original_build_redraw());
    MEM32(BUILD)=141;MEM32(node+24)=140;stronghold_original_build_click(node);
    stronghold_original_build_frame(1);stronghold_original_build_draw(node);assert(stronghold_original_build_help(0)==1);
    MEM32(OWNER+24)=1;stronghold_original_build_frame(1);stronghold_original_build_draw(node);
    assert(!stronghold_original_build_help(0));MEM32(OWNER+24)=0;
    MEM16(node+54)=1;stronghold_original_build_frame(1);stronghold_original_build_draw(node);assert(!help);
    MEM16(node+54)=0;MEM16(node+56)=1;stronghold_original_build_draw(node);assert(!help);
    MEM16(node+56)=0;stronghold_original_build_frame(0);stronghold_original_build_draw(node);
    assert(!stronghold_original_build_help(0)&&stronghold_original_build_help(7)==7&&!stronghold_original_build_redraw());
    stronghold_original_build_swipe(1);stronghold_original_build_frame(0);assert(stamp==1234);
    puts("HD construction: original tab eligibility/direction, guest-call register preservation, selected/rotated button hover+cost, modal/disabled/slide/desktop guards PASS");
    return 0;
}
