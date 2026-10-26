/* Production HD snapshots and guest-thread shortcuts; no game process is changed. */
#define STRONGHOLD_ORIGINAL
#include "../android/host/stronghold_original.c"
#include "../android/host/original_radar.c"
#include <assert.h>
#include <sys/mman.h>

static int calls, stamp = 1234, last_key, last_modifiers;
static int startup_calls;
static int native_zoom_calls, clamp_calls;
int stronghold_original_options_frame(void){return 0;}
int stronghold_text_frame(uint32_t state,uint32_t modal){(void)state;(void)modal;return 0;}
void stronghold_original_build_frame(int direct){(void)direct;}
void void_input_mouse_warp(int x,int y){(void)x;(void)y;}
void void_input_mouse_button(int button,int down){(void)button;(void)down;}
uint64_t void_monotonic_ns(void){return 1;}
uint32_t void_call_guest_this(uint32_t fn,uint32_t self,int count,const uint32_t *args)
{
    if(fn==0x4B5070u){
        assert(self==CAMERA&&count==1&&args[0]<=1);
        MEM32(CAMERA+0x8C)=args[0];MEM32(CAMERA+0x74)+=777;MEM32(CAMERA+0x78)+=999;
        ++native_zoom_calls;stamp=0;return 0;
    }
    if(fn==CAMERA_CLAMP){assert(self==CAMERA&&!count);++clamp_calls;stronghold_original_view(self);stamp=0;return 0;}
    assert(fn==0x45BEB0u&&self==0x1503D30u&&count==2&&args[0]==48&&args[1]==0);
    ++startup_calls;stamp=0;return 0;
}
static void zoom_camera(void)
{
    MEM32(STATE)=14;MEM32(STATE+4)=10;MEM32(DIALOG)=UINT32_MAX;
    MEM32(SCREEN_WIDTH)=1200;MEM32(SCREEN_HEIGHT)=600;
    MEM32(CAMERA+0x74)=3000;MEM32(CAMERA+0x78)=2000;MEM32(CAMERA+0x8C)=0;
    custom_width=0;pan_x=pan_y=0;zoom_camera_known=0;zoom_requested=1;
    world_zoom=unsnapped_zoom=.75;zoom_pending_x=-100;zoom_pending_y=-50;
    stronghold_display_smooth_camera(1);stronghold_original_frame();
    assert(native_zoom_calls==1&&clamp_calls==1&&stamp==1234);
    assert(MEM32(CAMERA+0x8C)==1&&MEM32(CAMERA+0x74)==2740&&MEM32(CAMERA+0x78)==1958);
    assert(MEM32(CAMERA+0x84)==60&&MEM32(CAMERA+0x88)==86); /* 800 x 315 source, doubled native cells. */
    assert(render_world_zoom==.75f&&!pan_x&&!pan_y);
    zoom_pending_x=64;zoom_pending_y=32;world_zoom=unsnapped_zoom=1.5;zoom_requested=1;
    stronghold_original_frame();
    assert(native_zoom_calls==2&&MEM32(CAMERA+0x8C)==0);
    assert(MEM32(CAMERA+0x74)==2964&&MEM32(CAMERA+0x78)==1982&&stamp==1234);
    assert(MEM32(CAMERA+0x84)==30&&MEM32(CAMERA+0x88)==43);
    /* Tiny world-pixel pans retain their residue in tile mode at either native zoom. */
    stronghold_display_smooth_camera(0);zoom_pending_x=7;zoom_pending_y=-5;
    stronghold_original_frame();assert(MEM32(CAMERA+0x74)==2964&&pan_x==-7&&pan_y==5);
    zoom_pending_x=25;zoom_pending_y=-11;stronghold_original_frame();
    assert(MEM32(CAMERA+0x74)==2996&&MEM32(CAMERA+0x78)==1966&&!pan_x&&!pan_y);
    /* Original keyboard Z changes the total zoom, without a stale GPU residual. */
    MEM32(CAMERA+0x8C)=1;stronghold_original_frame();assert(world_zoom==.5f&&render_world_zoom==.5f);
    world_zoom=unsnapped_zoom=render_world_zoom=1;zoom_requested=1;
    stronghold_original_frame();assert(MEM32(CAMERA+0x84)==43&&MEM32(CAMERA+0x88)==63);
    stronghold_display_smooth_camera(1);
    puts("HD zoom camera: original native mode rebuild, 160x8 raster-origin compensation, guest registers, scaled viewport/clamps, tile residues and keyboard parity PASS");
}
void void_log(const char *fmt,...){(void)fmt;}
int stronghold_original_build_panel(int x,int y){(void)x;(void)y;return 0;}
int void_vm_readable(uint32_t address, uint32_t size)
{ return address >= 0x400000 && (uint64_t)address + size <= 0x2000000; }
int void_vm_writable(uint32_t address,uint32_t size){return void_vm_readable(address,size);}
void void_regs_save(void_regs *r) { memset(r, 0, sizeof *r); r->eax = (uint32_t)stamp; }
void void_regs_load(const void_regs *r) { stamp = (int)r->eax; }
uint32_t void_call_guest(uint32_t fn, int count, const uint32_t *args, int pops)
{
    assert(fn == 0x4923D0 && count == 4 && pops == 1);
    ++calls; stamp = 0; last_key = (int)args[2];
    last_modifiers = (MEM32(0xAB49BC) ? 1 : 0) | (MEM32(0xAB49C4) ? 2 : 0);
    assert(!MEM32(0xAB49C0) && args[1] == (last_modifiers & 2 ? 0x104u : 0x100u));
    return 0;
}
int main(void)
{
    assert(mmap(GPTR(0x400000), 0x1C00000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    for (int factor=1; factor<=2; factor++) {
        MEM32(CAMERA + 0x8C) = factor - 1;
        stronghold_display_smooth_camera(0);
        pan_x=pan_y=0;
        int total_x=0,total_y=0,x,y;
        for (int i=0;i<64;i++) {
            pan_x+=1;pan_y+=.5f;take_pan(&x,&y);
            assert(x%32==0&&y%16==0);
            total_x+=x;total_y+=y;
        }
        assert(total_x==64&&total_y==32&&pan_x==0&&pan_y==0);
        for (int i=0;i<64;i++) {
            pan_x-=1;pan_y-=.5f;take_pan(&x,&y);
            total_x+=x;total_y+=y;
        }
        assert(!total_x&&!total_y&&pan_x==0&&pan_y==0);
        pan_x=3.5f;pan_y=-2.5f;
        stronghold_display_smooth_camera(1);take_pan(&x,&y);
        assert(x==3&&y==-2&&pan_x==.5f&&pan_y==-.5f);
        pan_x=pan_y=0;
    }
    puts("HD camera: tile steps, retained small/reverse motion at both zooms, live smooth toggle PASS");
    int values[SH_CONTROL_STATE];
    MEM32(STATE)=3;stronghold_game_ui_frame();assert(!startup_calls);
    stronghold_game_ui_skip(1);MEM32(STATE)=41;stronghold_game_ui_frame();assert(!startup_calls);
    MEM32(STATE)=26;stronghold_game_ui_frame();MEM32(STATE)=3;stronghold_game_ui_frame();
    assert(startup_calls==1&&stamp==1234);
    MEM32(STATE) = 41; MEM32(DIALOG) = UINT32_MAX;
    stronghold_game_control_state(values); assert(values[0] == 8 && !values[1] && !values[2]);
    stronghold_game_command('1', 3); original_commands(); assert(!calls);
    MEM32(STATE) = 14; MEM32(0x1503D84) = 1;
    const uint32_t units = 0xD47148, u = units + 1044 * 2, groups = 0xBCAFB8, marks = 0xBF5628;
    MEM32(units) = 4; MEM32(0x1008144) = 1;
    memset(GPTR(groups), 255, 120000);
    for (unsigned i = 0; i < 10; ++i) MEM32(marks + ('0' + i) * 4) = UINT32_MAX;
    MEM16(u + 716) = 1; MEM16(u + 804) = 2; MEM16(u + 806) = 22; MEM16(u + 814) = 1;
    MEM32(u + 816) = 123;
    MEM32(groups + 12000 + 8) = 2; MEM32(groups + 12000 + 12) = 123;
    MEM32(marks + '3' * 4) = 2345; MEM32(0x1503DE4) = 40;
    MEM32(SCREEN_WIDTH) = 1600; MEM32(SCREEN_HEIGHT) = 720;
    stronghold_game_control_state(values);
    assert(values[0] == 3 && values[1] == 1 && values[2] == 1 && values[3] == 4);
    assert(values[4] == 1600 && values[5] == 720 && values[9] == 40);
    /* Runtime dimensions come from the original sprite constructor. Deliberately
     * use a small book to catch a hard-coded artwork rectangle. */
    uint32_t node=0x5A16F0,owner=0x600000,radar=0x5A1240;
    MEM32(0xABB974)=owner;MEM32(owner+4)=268;MEM32(owner+8)=0;
    MEM32(node+76)=owner;MEM32(node+20)=0x43ADA0;MEM32(node+24)=71;
    MEM32(node+4)=686;MEM32(node+8)=503;MEM32(node+12)=37;MEM32(node+16)=23;
    MEM32(radar+76)=owner;MEM32(radar+20)=0x42D590;
    MEM32(radar+4)=571;MEM32(radar+8)=465;MEM32(radar+12)=126;MEM32(radar+16)=126;
    assert(original_hud_kind(970,510)==SH_TOUCH_BOOK&&original_hud_kind(993,510)==SH_TOUCH_UI);
    assert(original_hud_kind(850,480)==SH_TOUCH_RADAR);
    MEM16(node+54)=1;assert(original_hud_kind(970,510)==SH_TOUCH_UI);MEM16(node+54)=0;
    MEM16(node+56)=1;assert(original_hud_kind(970,510)==SH_TOUCH_UI);MEM16(node+56)=0;
    MEM32(node+76)=owner+80;assert(original_hud_kind(970,510)==SH_TOUCH_UI);MEM32(node+76)=owner;
    MEM32(owner+24)=1;assert(original_hud_kind(850,480)==SH_TOUCH_UI);MEM32(owner+24)=0;
    MEM32(DIALOG)=7;assert(original_hud_kind(850,480)==SH_TOUCH_UI);MEM32(DIALOG)=UINT32_MAX;
    MEM32(STATE+4)=60;assert(original_hud_kind(850,480)==SH_TOUCH_UI);
    MEM32(STATE+4)=62;assert(original_hud_kind(970,510)==SH_TOUCH_UI);MEM32(STATE+4)=10;
    current_kind=SH_TOUCH_BOOK;start_x=970;start_y=510;
    pthread_mutex_lock(&lock);original_hud_action_locked(SH_TOUCH_OPEN_TOOLS);pthread_mutex_unlock(&lock);
    assert(stronghold_game_ui_event()==4);
    current_kind=SH_TOUCH_RADAR;start_x=850;start_y=480;
    pthread_mutex_lock(&lock);original_hud_action_locked(SH_TOUCH_HOLD);pthread_mutex_unlock(&lock);
    assert(command_count==1&&command_keys[0]==0x20&&command_modifiers[0]==0);command_count=0;
    pthread_mutex_lock(&lock);original_hud_action_locked(SH_TOUCH_RADAR_TOGGLE);pthread_mutex_unlock(&lock);
    assert(command_count==1&&command_keys[0]=='R'&&command_modifiers[0]==2);command_count=0;
    MEM32(DIALOG)=7;
    pthread_mutex_lock(&lock);original_hud_action_locked(SH_TOUCH_HOLD);pthread_mutex_unlock(&lock);
    assert(!command_count);MEM32(DIALOG)=UINT32_MAX;
    uint32_t radar12=0x5B1370,book16=0x5A6B00;
    MEM32(STATE)=12;MEM32(0xABB974)=owner+80;
    MEM32(radar12+76)=owner+80;MEM32(radar12+20)=0x42D590;
    MEM32(radar12+4)=664;MEM32(radar12+8)=465;MEM32(radar12+12)=127;MEM32(radar12+16)=127;
    assert(original_hud_kind(670,470)==SH_TOUCH_RADAR);
    MEM32(STATE)=16;MEM32(STATE+4)=60;MEM32(0xABB974)=owner+160;
    MEM32(book16+76)=owner+160;MEM32(book16+20)=0x43ADA0;MEM32(book16+24)=71;
    MEM32(book16+4)=686;MEM32(book16+8)=503;MEM32(book16+12)=37;MEM32(book16+16)=23;
    assert(original_hud_kind(690,510)==SH_TOUCH_BOOK); /* state16 has no leading60/62 stop */
    MEM32(STATE)=14;MEM32(STATE+4)=10;MEM32(0xABB974)=owner;
    MEM16(u + 806) = 55; stronghold_game_control_state(values); assert(!values[1]);
    MEM32(u + 816) = 124; stronghold_game_control_state(values); assert(!values[2]);
    MEM32(u + 816) = 123; MEM16(u + 1336) = 1;
    stronghold_game_control_state(values); assert(!values[2]);
    MEM16(u + 1336) = 0; MEM16(u + 814) = 2;
    stronghold_game_control_state(values); assert(!values[2]);
    MEM32(units) = UINT32_MAX; stronghold_game_control_state(values); assert(!values[1] && !values[2]);
    MEM32(0xAB49BC) = 7; MEM32(0xAB49C0) = 8; MEM32(0xAB49C4) = 9;
    stronghold_game_command('1', 3); stronghold_game_command('Q', 0); stronghold_game_command('2', 4);
    original_commands(); assert(calls == 1 && last_key == '1' && last_modifiers == 3 && stamp == 1234);
    assert(MEM32(0xAB49BC) == 7 && MEM32(0xAB49C0) == 8 && MEM32(0xAB49C4) == 9);
    /* Radar colours have their own host flag, never the unrelated debug Alt+D. */
    MEM32(CAMERA+0xB8)=123;MEM32(0xBF6424+4)=7;MEM32(0xBF6424+8)=7;MEM32(0xBF6424+12)=8;
    assert(stronghold_original_radar_color(3,6)==6);
    stronghold_game_command('R', 2); original_commands(); assert(calls == 1&&stronghold_original_radar_colors());
    assert(MEM32(CAMERA+0xB8)==123&&MEM32(0x1012178)==1);
    assert(stronghold_original_radar_color(1,4)==8&&stronghold_original_radar_color(2,3)==8);
    assert(stronghold_original_radar_color(3,6)==2&&stronghold_original_radar_color(0,0)==0&&stronghold_original_radar_color(9,7)==7);
    stronghold_game_control_state(values);assert(values[8]&1);
    MEM32(0x1008144)=0;assert(stronghold_original_radar_color(2,3)==3);MEM32(0x1008144)=1;
    stronghold_game_command('R',2);original_commands();assert(!stronghold_original_radar_colors());
    assert(stronghold_original_radar_color(3,6)==6);
    for (int i = 0; i < 20; ++i) stronghold_game_command('Z', 0);
    original_commands(); assert(calls == 17 && !last_modifiers);
    MEM32(DIALOG) = 7; stronghold_game_command('1', 1); original_commands(); assert(calls == 17);
    zoom_camera();
    puts("HD controls: live groups/marks/speed, bounded original commands, runtime HUD book/radar bounds+guards, lock-owned actions, team radar colours/neutral/format-independent index, original debug state/registers preserved PASS");
    return 0;
}
