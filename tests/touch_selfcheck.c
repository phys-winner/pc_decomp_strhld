/* Standalone ownership/queue tests use the actual native resolver. Original
 * x86 pick/permission/order semantics are covered by tools/test_touch.py. */
#include "../android/host/touch.c"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

ptrdiff_t g_mem_base=(ptrdiff_t)VOID_GUEST_BASE;
static int guest_calls, picks, clears, groups, context_calls, orders, selections;
static int operation, serial, context_serial, pick_serial, input_events;
static int button_down,button_up;
static int pause_closes,advantage_value,advantage_calls;
static int scroll_min,scroll_max,scroll_current,scroll_refresh,scroll_commit,scroll_wheel;
static int chat_current,chat_blocked;
static int box_draw_calls, cancels, inspection_closes;
static uint32_t box_draw_args[2][5];
static float ui_inverse=1;
static int tools_opened,command_key,command_modifiers,commands,zoom_sessions,zoom_resets,keep_state;
static int highlight[4];
void stronghold_game_tools(void){tools_opened++;}
void stronghold_game_command(int key,int modifiers){command_key=key;command_modifiers=modifiers;++commands;}
void stronghold_game_ui_click(uint32_t node){(void)node;}
void stronghold_game_ui_frame(void){}
static int feedback_bits;
void stronghold_feedback(int bits){feedback_bits|=bits;}
void void_log(const char *fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);putchar('\n');va_end(a);}
int void_vm_readable(uint32_t a,uint32_t n){return a>=0x400000u&&(uint64_t)a+n<0x3400000u;}
int void_vm_writable(uint32_t a,uint32_t n){return void_vm_readable(a,n);}
void void_regs_save(void_regs *r){memset(r,0,sizeof *r);}
void void_regs_load(const void_regs *r){(void)r;}
uint32_t void_call_guest_this(uint32_t fn,uint32_t self,int n,const uint32_t *args)
{
    (void)self;(void)n;(void)args;++guest_calls;++serial;
    if(fn==A(0x493F70,0x494100)) {
        assert(self==A(0x11639F0,0x1163E50)&&!n);++pause_closes;
        MEM32(A(0x2125920,0x2BB8E00)+44)=UINT32_MAX;return 0;
    }
    if(fn==A(0x46B430,0x46B610)) {
        assert(self==GAMEOBJ&&n==2&&args[0]==14&&args[1]==100);
        ++inspection_closes;return 0;
    }
    if(fn==A(0x471750,0x471960)) {
        uint32_t drawing=A(0x1A55350,0x248C3A0);
        assert(self==drawing&&n==5&&MEM32(drawing+12)==1);
        assert(box_draw_calls<2);
        memcpy(box_draw_args[box_draw_calls++],args,sizeof box_draw_args[0]);
        /* Original468E00/469000 refreshes the primitive context. Passing the
         * TGX renderer here corrupts its mode with this8112-byte pitch. */
        MEM32(self+4)=0x03004000;MEM32(self+8)=8112;
        return 1;
    }
    if(fn==KEEP_STATE){assert(self==KEEP_OWNER&&n==0);return (uint32_t)keep_state;}
    if(fn==A(0x4E8DD0,0x4E9180)){MEM32(MAP)=1;return 0;}
    if(fn==PICK){++picks;pick_serial=serial;return 1;}
    if(fn==CLEAR){++clears;return 0;}
    if(fn==CLEAR_GROUP){++groups;return 0;}
    if(fn==INSPECT&&stronghold_touch_cancel()) {++cancels;MEM32(BUILD)=0;return 0;}
    if(fn==CONTEXT){
        ++context_calls;
        if(!context_serial)context_serial=serial;
        MEM32(A(0x2125870,0x2BB8D50))=operation;
        if(stronghold_touch_selection())++selections;
        else if(stronghold_touch_order(2))++orders;
    }
    return 0;
}
uint32_t void_call_guest(uint32_t fn,int n,const uint32_t *args,int stdcall_pops)
{
    (void)fn;assert(stdcall_pops==0);
    if(fn==ADVANTAGE){assert(n==1&&args[0]>=10&&args[0]<=14);advantage_value=args[0];++advantage_calls;return 0;}
    if(n==1){
        if(fn==A(0x444D00,0x444F40)){MEM32(GAMESTATE+4)=args[0];return 0;}
        if(fn==CHAT_SCROLL){
            assert(args[0]==UINT32_MAX||args[0]==UINT32_MAX-1);
            if(!chat_blocked){if(args[0]==UINT32_MAX&&chat_current<15)++chat_current;else if(args[0]==UINT32_MAX-1&&chat_current>0)--chat_current;}
        } else {assert(args[0]==1||args[0]==2);scroll_wheel+=args[0]==2?1:-1;}
        return 0;
    }
    assert(n==5&&args[1]!=1); /* event1 is initialization, not a getter. */
    if(args[1]==4){++scroll_refresh;MEM32(args[2])=scroll_min;MEM32(args[3])=scroll_max;MEM32(args[4])=scroll_current;}
    else {assert(args[1]==3);++scroll_commit;scroll_current=(int32_t)MEM32(args[4]);}
    return 0;
}
void void_input_mouse_button(int b,int down){(void)b;++input_events;if(down)++button_down;else++button_up;}
void void_input_mouse_warp(int x,int y){(void)x;(void)y;++input_events;}
static int wheel_steps,wheel_sum,test_edges=-1;
int void_input_edges(void){return test_edges;}
void void_input_mouse_wheel(int d){++input_events;++wheel_steps;wheel_sum+=d;}
void stronghold_display_set_ui_scale(float f){(void)f;}
void stronghold_display_zoom_session(void){++zoom_sessions;}
void stronghold_display_zoom_reset(void){++zoom_resets;}
void stronghold_display_highlight(int l,int t,int w,int h){highlight[0]=l;highlight[1]=t;highlight[2]=w;highlight[3]=h;}
static int transforms;static float transform_dx,transform_dy,transform_scale;
void stronghold_display_transform(float x,float y,float dx,float dy,float z){(void)x;(void)y;++transforms;transform_dx=dx;transform_dy=dy;transform_scale=z;}
int stronghold_display_map_point(float x,float y,int *gx,int *gy){*gx=(int)x;*gy=(int)y;return 1;}
int stronghold_display_world_point(float x,float y,int *gx,int *gy){*gx=(int)x;*gy=(int)y;return 1;}
int stronghold_display_ui_point(int g,float x,float y,int *gx,int *gy){(void)g;*gx=(int)x;*gy=(int)y;return 1;}
float stronghold_display_ui_dx(float x){return x*ui_inverse;}
float stronghold_display_ui_dy(float y){return y*ui_inverse;}
uint32_t stronghold_display_width(uint32_t original){(void)original;return 1560;}
uint32_t stronghold_display_height(uint32_t original){(void)original;return 720;}

static void fixture(int target_operation)
{
    sequence=7;atomic_store(&requested_sequence,7);atomic_store(&published,(uint64_t)7<<32);atomic_store(&enabled,1);
    classification=SH_TOUCH_WORLD;
    aim_known=1;aim_x=310;aim_y=210;action=SH_TOUCH_TAP;commit_consumed=selection_kind=0;
    callbacks=1u<<CALLBACK_CONTEXT;
    MEM32(GAMESTATE)=14;MEM32(MODE)=1;MEM32(BUILD)=0;
    MEM32(UNITS)=2;MEM32(UNITS+16)=1;MEM32(PLAYER)=1;
    MEM16(UNITS+1168+1706)=1;
    guest_calls=picks=clears=groups=context_calls=orders=selections=serial=context_serial=pick_serial=0;
    operation=target_operation;
}

static void box_receiver_checks(void)
{
    uint32_t renderer=A(0x21288D8,0x2BBBDB8),drawing=A(0x1A55350,0x248C3A0);
    unsigned char before[256],saved[256];
    memcpy(saved,GPTR(renderer),sizeof saved);
    memset(GPTR(renderer),0xA6,sizeof before);
    MEM32(renderer+4)=0;MEM32(renderer+8)=1;MEM32(renderer+12)=0x03004000;
    memcpy(before,GPTR(renderer),sizeof before);
    MEM16(A(0xDF5418,0xDF5598))=0x1234;
    MEM16(A(0xDF53D4,0xDF5554))=0x5678;
    MEM32(MAP+0x80)=96;MEM32(MAP+0x84)=48;
    box_x=140;box_y=240;box_end_x=40;box_end_y=80;
    for(int zoom=0;zoom<2;zoom++)for(int prior=0;prior<2;prior++) {
        MEM32(MAP+0x90)=zoom;MEM32(drawing+12)=prior;box_draw_calls=0;
        draw_box();
        uint32_t outer[]={136,128,236,288,0x1234},inner[]={137,129,235,287,0x5678};
        assert(box_draw_calls==2);
        assert(!memcmp(box_draw_args[0],outer,sizeof outer));
        assert(!memcmp(box_draw_args[1],inner,sizeof inner));
        assert(MEM32(drawing+12)==(uint32_t)prior);
        assert(MEM32(drawing+8)==8112);
        assert(!memcmp(before,GPTR(renderer),sizeof before));
    }
    memcpy(GPTR(renderer),saved,sizeof saved);
    MEM32(MAP+0x80)=MEM32(MAP+0x84)=MEM32(MAP+0x90)=0;
    puts("native BOX: original primitive receiver, both outlines/colors, origin/zoom, flag restoration, TGX256bytes intact PASS");
}

static void list_checks(void)
{
    const uint32_t owner=0x3300000;
    for(unsigned f=0;f<sizeof touch_lists/sizeof *touch_lists;f++) {
        const touch_list_family *family=&touch_lists[f];
        uint32_t row=family->first,bar=family->bar;
        memset(GPTR(owner),0,80);memset(GPTR(row),0,160);memset(GPTR(bar),0,80);
        MEM32(owner)=row;MEM32(row)=3;MEM32(row+4)=10;MEM32(row+8)=20;
        MEM32(row+12)=200;MEM32(row+16)=20;MEM32(row+76)=owner;MEM32(row+80)=102;
        MEM32(bar)=6;MEM32(bar+20)=family->callback;MEM32(bar+76)=owner;
        MEM32(bar+12)=20;MEM32(bar+16)=220;
        assert(widget_class(owner,100,25)==SH_TOUCH_LIST&&ui_scroll_node==bar&&ui_scroll_family==family);
        scroll_min=0;scroll_max=1000;scroll_current=100;scroll_refresh=scroll_commit=0;
        ui_scroll_last=0;ui_inverse=1;
        int pixels=family->pixels,units=family->units;
        scroll_ui(0,-pixels*.4f);assert(scroll_current==100&&scroll_commit==0);
        scroll_ui(0,-pixels*.6f);assert(scroll_current==100+units&&scroll_commit==1);
        scroll_ui(0,-pixels*2);assert(scroll_current==100+3*units&&scroll_commit==2);
        scroll_ui(0,pixels);assert(scroll_current==100+2*units&&scroll_commit==3);
        scroll_ui(0,-100000);assert(scroll_current==1000);
        scroll_ui(0,100000);assert(scroll_current==0);
        ui_scroll_pixels=0;scroll_current=100;ui_inverse=2;
        scroll_ui(0,-pixels*.5f);assert(scroll_current==100+units);
        /* A disabled/moved/mismatched bar cannot gain LIST semantics. */
        MEM16(bar+54)=1;assert(widget_class(owner,100,25)==SH_TOUCH_UI);MEM16(bar+54)=0;
        MEM32(bar+20)=family->callback+1;assert(widget_class(owner,100,25)==SH_TOUCH_UI);
    }
    /* Generic horizontal sliders follow their thumb, not content direction. */
    uint32_t bar=0x3300100;memset(GPTR(bar),0,80);
    MEM32(bar)=5;MEM32(bar+12)=220;MEM32(bar+16)=20;MEM32(bar+20)=0x401000;MEM32(bar+32)=20;
    ui_scroll_node=bar;ui_scroll_last=0;ui_scroll_family=NULL;ui_inverse=1;
    scroll_min=0;scroll_max=10;scroll_current=4;
    scroll_ui(0,100);assert(scroll_current==4);
    scroll_ui(40,0);assert(scroll_current==6);
    scroll_ui(-20,0);assert(scroll_current==5);
    /* Wheel-only rows have one callback per row-height, independent of MOVE count. */
    MEM32(bar)=3;MEM32(bar+36)=2;scroll_wheel=0;ui_scroll_last=0;
    scroll_ui(0,-9);scroll_ui(0,-11);assert(scroll_wheel==1);
    scroll_ui(0,-40);assert(scroll_wheel==3);
    scroll_ui(0,20);assert(scroll_wheel==2);
    /* Chat uses its original reverse-history callback, not the no-op body. */
    memset(GPTR(CHAT_BODY),0,160);MEM32(owner)=CHAT_BODY;MEM32(CHAT_BODY)=3;
    MEM32(CHAT_BODY+4)=10;MEM32(CHAT_BODY+8)=20;MEM32(CHAT_BODY+12)=200;MEM32(CHAT_BODY+16)=107;
    MEM32(CHAT_BODY+20)=0x401000;MEM32(CHAT_BODY+24)=UINT32_MAX-2;
    MEM32(CHAT_BODY+28)=A(0x42AD40,0x42ADC0);MEM32(CHAT_BODY+76)=owner;MEM32(CHAT_BODY+80)=102;
    MEM32(A(0x1A559B0,0x248CA00))=0;MEM32(A(0x212594C,0x2BB8E2C))=UINT32_MAX;
    assert(widget_class(owner,100,25)==SH_TOUCH_LIST&&ui_scroll_node==CHAT_BODY);
    MEM32(A(0x1A559B0,0x248CA00))=99;assert(widget_class(owner,100,25)==SH_TOUCH_UI);
    MEM32(A(0x1A559B0,0x248CA00))=0;MEM32(A(0x212594C,0x2BB8E2C))=0;
    assert(widget_class(owner,100,25)==SH_TOUCH_UI);
    MEM32(A(0x212594C,0x2BB8E2C))=UINT32_MAX;
    assert(widget_class(owner,100,25)==SH_TOUCH_LIST);
    ui_scroll_last=0;chat_current=0;chat_blocked=0;
    scroll_ui(0,8);scroll_ui(0,9);assert(chat_current==1);
    scroll_ui(0,34);assert(chat_current==3);
    scroll_ui(0,-17);assert(chat_current==2);
    scroll_ui(0,1700);assert(chat_current==15);
    scroll_ui(0,-17);assert(chat_current==14); /* no stale backlog at clamp */
    scroll_ui(0,-1700);assert(chat_current==0);
    chat_blocked=1;scroll_ui(0,34);assert(chat_current==0);chat_blocked=0;
    ui_inverse=1;ui_scroll_node=ui_scroll_last=0;ui_scroll_family=NULL;
}

/* A USB/Bluetooth mouse: WHEEL over the match world zooms around the cursor
 * and re-arms the 100% detent per burst; over the HUD or a menu the original
 * wheel gets whole notches, one per frame. GRAB pans only in a match. */
static void mouse_checks(void)
{
    const uint32_t roots[]={A(0x2598E60,0x30D6200),A(0x179FB28,0x20D2B50),A(0x2125920,0x2BB8E00)};
    for(unsigned i=0;i<3;i++)MEM32(roots[i]+44)=UINT32_MAX;
    MEM32(UI_ROOT)=0;MEM32(A(0x2128940,0x2BBBE20))=0;MEM32(BUILD)=0;MEM32(MODE)=1;
    MEM32(A(0x212889C,0x2BBBD7C))=MEM32(A(0x1A55F30,0x248CF80))=MEM32(A(0x1A56054,0x248D0A4))=0;
    MEM32(GAMESTATE)=14;MEM32(GAMESTATE+4)=0;
    ui_drain=ui_release=ui_held=0;queue_head=queue_count=0;wheel_pending=0;
    for(int mode=1;mode>=0;mode--) {
        atomic_store(&enabled,mode);
        transforms=wheel_steps=wheel_sum=zoom_sessions=0;
        ui_x=10;ui_y=20;ui_hit_active=1;aim_x=11;aim_y=21;ui_tracking=1; /* a finger hovering the UI */
        stronghold_touch_enqueue(SH_TOUCH_WHEEL,0,300,100,120,1,1.15f);
        stronghold_touch_frame();
        assert(transforms==1&&fabsf(transform_scale-1.15f)<1e-5f&&!transform_dx&&!transform_dy&&zoom_sessions==1&&!wheel_steps);
        assert(ui_x==10&&ui_y==20&&ui_hit_active==1&&aim_x==11&&aim_y==21); /* a live touch keeps its point */
        ui_tracking=0;
        stronghold_touch_enqueue(SH_TOUCH_WHEEL,0,300,100,0,0,1.02f);
        stronghold_touch_frame();assert(transforms==2&&zoom_sessions==1); /* same burst: the detent is not re-armed */
        stronghold_touch_enqueue(SH_TOUCH_WHEEL,0,300,650,120,1,1.15f);
        stronghold_touch_enqueue(SH_TOUCH_WHEEL,0,300,650,120,0,1.15f);
        assert(queue_count==1);
        stronghold_touch_frame();assert(transforms==2&&!wheel_steps&&wheel_pending==240);
        stronghold_touch_frame();assert(wheel_steps==1&&wheel_sum==120);
        stronghold_touch_frame();assert(wheel_steps==2&&wheel_sum==240);
        stronghold_touch_frame();assert(wheel_steps==2&&!wheel_pending);
        stronghold_touch_enqueue(SH_TOUCH_GRAB,0,300,100,.01f,.02f,1);
        stronghold_touch_enqueue(SH_TOUCH_GRAB,0,310,100,.01f,0,1);
        stronghold_touch_frame();
        assert(transforms==3&&fabsf(transform_dx-.02f)<1e-6f&&fabsf(transform_dy-.02f)<1e-6f&&transform_scale==1);
    }
    /* Desktop edge scrolling follows the physical screen edge; direct touch never scrolls. */
    uint32_t flags=A(0x11684C0,0x1168920);
    for(int i=0;i<4;i++)MEM32(flags+4*i)=1;
    atomic_store(&enabled,0);test_edges=-1;stronghold_touch_frame();
    for(int i=0;i<4;i++)assert(MEM32(flags+4*i)==1); /* no pointer yet: the original's own result */
    test_edges=1|8;stronghold_touch_frame();
    assert(MEM32(flags)==1&&MEM32(flags+4)==0&&MEM32(flags+8)==0&&MEM32(flags+12)==1);
    test_edges=2|4;stronghold_touch_frame();
    assert(MEM32(flags)==0&&MEM32(flags+4)==1&&MEM32(flags+8)==1&&MEM32(flags+12)==0);
    atomic_store(&enabled,1);stronghold_touch_frame();
    for(int i=0;i<4;i++)assert(MEM32(flags+4*i)==0);
    test_edges=-1;atomic_store(&enabled,0);
    MEM32(GAMESTATE)=41;transforms=wheel_steps=wheel_sum=0;
    stronghold_touch_enqueue(SH_TOUCH_WHEEL,0,300,100,-120,1,.87f);
    stronghold_touch_frame();stronghold_touch_frame();assert(!transforms&&wheel_steps==1&&wheel_sum==-120);
    stronghold_touch_enqueue(SH_TOUCH_GRAB,0,300,100,.01f,.02f,1);stronghold_touch_frame();assert(!transforms);
    stronghold_touch_enqueue(SH_TOUCH_WHEEL,0,300,100,600,1,1);stronghold_touch_frame();assert(wheel_pending==600);
    stronghold_touch_abort();stronghold_touch_frame();assert(!wheel_pending); /* an OS abort drops owed notches */
    atomic_store(&enabled,1);assert(!stronghold_touch_desktop());
    stronghold_touch_enqueue(SH_TOUCH_MODE,0,0,0,0,0,0);assert(!stronghold_touch_desktop()); /* applied by the game thread */
    stronghold_touch_frame();assert(stronghold_touch_desktop());
    stronghold_touch_enqueue(SH_TOUCH_MODE,0,0,0,0,0,1);stronghold_touch_frame();assert(!stronghold_touch_desktop());
    MEM32(GAMESTATE)=0;
    puts("native mouse: world wheel zoom with per-burst detent, whole original notches one per frame over HUD/menus, grab pan in matches only, abort, desktop-mode getter, physical-edge scrolling PASS");
}

static void spawn_drag_checks(void)
{
    fixture(0);abort_gesture();ui_drain=0;queue_head=queue_count=0;
    const uint32_t owner=0x3300000,node=owner+256,map=node+160;
    memset(GPTR(owner),0,1024);MEM32(owner)=node;
    MEM32(owner+4)=130;MEM32(owner+8)=50;
    /* Actual 5EBB68/5EBBB8 setup map controls cover the draw-only record. */
    for(unsigned i=0;i<2;i++) {
        uint32_t control=node+80*i;
        MEM32(control)=i?0x0200000Au:0x02000003u;
        MEM32(control+4)=578;MEM32(control+8)=13;MEM32(control+12)=MEM32(control+16)=200;
        MEM32(control+20)=CHAT_SCROLL;MEM32(control+76)=owner;
    }
    MEM32(map)=3;MEM32(map+28)=A(0x4281B0,0x428230);MEM32(map+76)=owner;MEM32(map+80)=102;
    MEM32(UI_ROOT)=owner;MEM32(GAMESTATE)=0;
    const uint32_t roots[]={A(0x2598E60,0x30D6200),A(0x179FB28,0x20D2B50),A(0x2125920,0x2BB8E00)};
    for(unsigned i=0;i<3;i++)MEM32(roots[i]+44)=UINT32_MAX;
    MEM32(A(0x1A55B28,0x248CB78))=1;
    MEM32(A(0xFD57A0,0xFD5C00))=130;MEM32(A(0xFD57A4,0xFD5C04))=50;
    for(unsigned i=0;i<8;i++)MEM32(A(0x21282A4,0x2BBB784)+8*i)=UINT32_MAX;
    MEM32(A(0x21282A4,0x2BBB784))=10;MEM32(A(0x21282A8,0x2BBB788))=20;
    assert(point(712,73)==SH_TOUCH_DRAG&&point(735,105)==SH_TOUCH_DRAG);
    assert(point(688,40)==SH_TOUCH_DRAG&&point(759,138)==SH_TOUCH_DRAG);
    assert(point(760,138)==SH_TOUCH_UI&&point(759,139)==SH_TOUCH_UI&&point(20,20)==SH_TOUCH_UI);
    MEM32(A(0x21282A4,0x2BBB784)+8)=50;MEM32(A(0x21282A8,0x2BBB788)+8)=20;
    assert(spawn_icon(743,89)==0&&spawn_icon(745,89)==1);
    MEM32(A(0x21282A4,0x2BBB784)+8)=UINT32_MAX;
    MEM16(node+54)=1;assert(point(712,73)==SH_TOUCH_DRAG);MEM16(node+54)=0;
    MEM16(map+54)=1;assert(point(712,73)==SH_TOUCH_UI);MEM16(map+54)=0;
    MEM32(node+20)=123;assert(point(712,73)==SH_TOUCH_UI);MEM32(node+20)=CHAT_SCROLL;
    const uint32_t other=owner+900;
    MEM32(other+4)=130;MEM32(other+8)=50;MEM32(node+76)=other;
    assert(point(712,73)==SH_TOUCH_UI);MEM32(node+76)=owner;
    MEM32(roots[0]+44)=1;MEM32(roots[0]+8)=700;MEM32(roots[0]+12)=60;
    MEM32(roots[0]+16)=100;MEM32(roots[0]+20)=100;MEM32(roots[0]+36)=0;
    assert(point(712,73)==SH_TOUCH_UI);MEM32(roots[0]+44)=UINT32_MAX;
    MEM32(A(0x1A55B28,0x248CB78))=0;assert(point(712,73)==SH_TOUCH_UI);
    MEM32(A(0x1A55B28,0x248CB78))=1;
    int down=button_down,up=button_up;
    stronghold_touch_enqueue(SH_TOUCH_QUERY,91,716,80,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_BEGIN,91,716,80,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_UPDATE,91,800,150,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_END,91,810,160,0,0,1);
    stronghold_touch_frame();assert(ui_dragging&&button_down==down+1&&button_up==up&&!ui_hover_guard);
    stronghold_touch_frame();assert(ui_dragging&&ui_x==800&&ui_y==150&&button_up==up);
    stronghold_touch_frame();assert(!ui_dragging&&button_up==up+1&&ui_x==810&&ui_y==160&&ui_drain==2);
    ui_drain=0;
    stronghold_touch_enqueue(SH_TOUCH_QUERY,92,716,80,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_BEGIN,92,716,80,0,0,1);
    stronghold_touch_frame();assert(ui_dragging);
    MEM32(A(0xB975C0,0xB97740))=1;
    stronghold_touch_enqueue(SH_TOUCH_ABORT,92,800,150,0,0,1);
    stronghold_touch_frame();assert(!ui_dragging&&MEM32(A(0xB975C0,0xB97740))==UINT32_MAX&&button_up==up+2);
    ui_drain=0;
    stronghold_touch_enqueue(SH_TOUCH_QUERY,93,716,80,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_BEGIN,93,716,80,0,0,1);
    stronghold_touch_frame();assert(ui_dragging);
    MEM32(A(0xB975C0,0xB97740))=1;
    MEM32(roots[0]+44)=1;MEM32(roots[0]+8)=790;MEM32(roots[0]+12)=140;
    MEM32(roots[0]+16)=100;MEM32(roots[0]+20)=100;MEM32(roots[0]+36)=0;
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_UPDATE,93,800,150,0,0,1);
    stronghold_touch_frame();assert(!ui_dragging&&ui_hover_guard&&ui_x==-1&&ui_y==-1);
    assert(MEM32(A(0xB975C0,0xB97740))==UINT32_MAX&&button_up==up+3);
    MEM32(roots[0]+44)=UINT32_MAX;
    ui_drain=0;
    stronghold_touch_enqueue(SH_TOUCH_QUERY,94,716,80,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_BEGIN,94,716,80,0,0,1);
    stronghold_touch_frame();assert(ui_dragging);
    MEM32(A(0xB975C0,0xB97740))=1;MEM32(node+20)=123;
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_UPDATE,94,800,150,0,0,1);
    stronghold_touch_frame();assert(!ui_dragging&&button_up==up+4);
    assert(MEM32(A(0xB975C0,0xB97740))==UINT32_MAX);MEM32(node+20)=CHAT_SCROLL;
    MEM32(UI_ROOT)=0;ui_drain=0;fixture(0);
    puts("native spawn drag: active original minimap only, inclusive flag bounds, FIFO press/move/release, OS/modal abort clears original drag without a swap PASS");
}

static void menu_slider_radar_checks(void)
{
    fixture(0);abort_gesture();ui_drain=0;queue_head=queue_count=0;
    uint32_t root=A(0x2125920,0x2BB8E00),owner=0x3300000,node=owner+256;
    MEM32(A(0x2598E60,0x30D6200)+44)=UINT32_MAX;
    MEM32(A(0x179FB28,0x20D2B50)+44)=UINT32_MAX;
    memset(GPTR(owner),0,1024);MEM32(owner)=node;MEM32(node)=102;
    MEM32(root+8)=100;MEM32(root+12)=100;MEM32(root+16)=300;MEM32(root+20)=300;
    MEM32(root+36)=owner;MEM32(root+44)=5;
    assert(point(20,20)==SH_TOUCH_MENU&&point(200,200)==SH_TOUCH_MENU);
    MEM32(owner+4)=250;MEM32(owner+8)=124;
    MEM32(node)=3;MEM32(node+4)=20;MEM32(node+8)=20;MEM32(node+12)=80;MEM32(node+16)=20;
    MEM32(node+20)=123;MEM32(node+76)=owner;MEM32(node+80)=102;
    assert(point(130,130)==SH_TOUCH_MENU&&ui_node==node);
    int down=button_down;
    stronghold_touch_enqueue(SH_TOUCH_QUERY,95,20,20,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_CLICK,95,20,20,0,0,1);stronghold_touch_frame();
    assert(pause_closes==1&&button_down==down);
    MEM32(root+44)=UINT32_MAX;MEM32(GAMESTATE)=20;MEM32(UI_ROOT)=owner;
    MEM32(owner+4)=MEM32(owner+8)=0;
    for(int i=0;i<5;i++){
        uint32_t p=node+80*i;MEM32(p)=3;MEM32(p+4)=20+70*i;MEM32(p+8)=243;
        MEM32(p+12)=60;MEM32(p+16)=27;MEM32(p+20)=ADVANTAGE;MEM32(p+24)=10+i;MEM32(p+76)=owner;
    }
    MEM32(node+400)=102;
    assert(point(175,250)==SH_TOUCH_DRAG);
    stronghold_touch_enqueue(SH_TOUCH_QUERY,96,175,250,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_BEGIN,96,175,250,0,0,1);stronghold_touch_frame();
    assert(advantage_value==12&&advantage_calls==1);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_UPDATE,96,333,250,0,0,1);stronghold_touch_frame();
    assert(advantage_value==14&&advantage_calls==2);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_END,96,333,250,0,0,1);stronghold_touch_frame();
    assert(advantage_calls==2&&!slider_owner&&button_down==down);
    MEM32(root+44)=0;MEM32(root+8)=MEM32(root+12)=0;MEM32(root+16)=500;MEM32(root+20)=400;
    stronghold_touch_enqueue(SH_TOUCH_QUERY,97,333,250,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_BEGIN,97,333,250,0,0,1);stronghold_touch_frame();
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_UPDATE,97,295,250,0,0,1);stronghold_touch_frame();
    assert(slider_owner==owner&&advantage_value==13);
    stronghold_touch_enqueue(SH_TOUCH_UI_DRAG_END,97,50,250,0,0,1);stronghold_touch_frame();
    assert(advantage_value==10&&!slider_owner&&button_down==down);
    MEM32(root+44)=UINT32_MAX;
    MEM32(UI_ROOT)=0;fixture(0);abort_gesture();classification=SH_TOUCH_RADAR;transforms=0;
    stronghold_touch_enqueue(SH_TOUCH_PAN,7,900,650,.1f,.2f,1);stronghold_touch_frame();
    assert(transforms==1&&transform_dx==.2f&&transform_dy==.4f);
    MEM32(root+44)=11;
    stronghold_touch_enqueue(SH_TOUCH_PAN,7,900,650,.1f,.2f,1);stronghold_touch_frame();
    assert(transforms==1);MEM32(root+44)=UINT32_MAX;
    puts("pause outside resumes without world click, active original advantage callbacks/clamped nearest step, relative radar pan PASS");
}

int main(void)
{
    void *memory=mmap((void *)(VOID_GUEST_BASE+0x400000),0x3000000,PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    assert(memory!=MAP_FAILED);
    assert(stronghold_touch_selftest()==0);
    menu_slider_radar_checks();
    box_receiver_checks();
    list_checks();
    /* OS abort and world gestures hide the old HUD cursor in direct mode;
     * legacy touchpad uses its real cursor even with a stored native point. */
    atomic_store(&enabled,1);ui_hit_active=0;abort_gesture();
    assert(stronghold_touch_ui_x(123)==UINT32_MAX&&stronghold_touch_ui_y(456)==UINT32_MAX);
    atomic_store(&enabled,0);
    assert(stronghold_touch_ui_x(123)==123&&stronghold_touch_ui_y(456)==456);
    /* The ordinary same-owner target is not selected before the engine has
     * evaluated a specific context action (original operation3/queue36). */
    fixture(3);stronghold_touch_after_world();
    assert(context_calls==2&&orders==1&&picks==0&&clears==0&&selections==0);
    fixture(4);stronghold_touch_after_world();
    assert(context_calls==2&&orders==1&&picks==0&&clears==0&&selections==0);
    fixture(0);stronghold_touch_after_world();
    assert(context_calls==2&&orders==0&&picks==1&&clears==1&&groups==1&&selections==1);
    assert(context_serial<pick_serial);
    /* A completed TAP remains FIFO-owned even when the next DOWN has queued
     * QUERY8. Its result must not overwrite the latest pending QUERY status. */
    fixture(3);stronghold_touch_enqueue(SH_TOUCH_TAP,7,310,210,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_QUERY,8,320,220,0,0,1);
    stronghold_touch_frame();callbacks=1u<<CALLBACK_CONTEXT;
    stronghold_touch_after_world();
    assert(orders==1&&sequence==7&&stronghold_touch_status(8)==SH_TOUCH_PENDING);
    stronghold_touch_frame();assert(sequence==8&&stronghold_touch_status(8)==SH_TOUCH_WORLD);
    guest_calls=orders=0;
    stronghold_touch_enqueue(SH_TOUCH_TAP,7,310,210,0,0,1);
    stronghold_touch_frame();stronghold_touch_after_world();assert(orders==0);
    /* OS abort itself is lock-free and touches neither the engine nor input. */
    fixture(3);int before=input_events;stronghold_touch_abort();
    assert(guest_calls==0&&input_events==before&&atomic_load(&abort_requested));
    atomic_store(&abort_requested,0);
    /* Even a fully transparent pixel inside a live powers/modal rectangle
     * receives first refusal: the display's pixel hint always returnsWORLD. */
    MEM32(A(0x2598E60,0x30D6200)+44)=UINT32_MAX;
    uint32_t panel=A(0x179FB28,0x20D2B50);
    MEM32(panel+44)=130;MEM32(panel+8)=1505;MEM32(panel+12)=56;
    MEM32(panel+16)=55;MEM32(panel+20)=320;
    assert(point(1520,100)==SH_TOUCH_UI);
    assert(point(1504,100)==SH_TOUCH_WORLD);
    assert(point(1560,100)==SH_TOUCH_WORLD);
    MEM32(MAP)=0;assert(point(1225,100)==SH_TOUCH_WORLD);
    /* Exact original layout1 portrait rect #5 from B477D0/B47640. It is
     * above the reserved bottom128 and is blank according to pixelhint. */
    MEM32(A(0x2128940,0x2BBBE20))=1;
    uint32_t table=A(0xB47640,0xB477D0);
    MEM32(table)=768;MEM32(table+4)=402;MEM32(table+8)=27;MEM32(table+12)=16;
    MEM32(table+16)=UINT32_MAX;
    MEM32(A(0xF687E8,0xF68C48))=380;MEM32(A(0xF687EC,0xF68C4C))=120;
    assert(point(1150,525)==SH_TOUCH_UI);
    assert(point(1175,538)==SH_TOUCH_UI); /* original inclusive right/bottom */
    assert(point(1176,538)==SH_TOUCH_WORLD);
    assert(point(1175,539)==SH_TOUCH_WORLD);
    /* Tooltip HOLD/END/OS abort produce no mouse button transitions, even
     * with a stale original release predicate; next explicit CLICK works. */
    MEM32(GAMESTATE)=0;ui_drain=ui_release=ui_held=0;
    sequence=80;classification=SH_TOUCH_UI;
    atomic_store(&requested_sequence,80);atomic_store(&published,((uint64_t)80<<32)|SH_TOUCH_UI);
    int down=button_down,up=button_up;
    stronghold_touch_enqueue(SH_TOUCH_UI_HOLD_BEGIN,80,10,20,0,0,1);
    stronghold_touch_frame();assert(ui_held&&ui_hover_guard&&stronghold_touch_ui_button(1)==0);
    uint32_t redraw=A(0x2125A60,0x2BB8F40),help=A(0x2125A6C,0x2BB8F4C);
    MEM32(redraw)=2;MEM32(help)=0;
    assert(stronghold_touch_ui_redraw()&&stronghold_touch_ui_help(MEM32(help))==1);
    stronghold_touch_frame();assert(ui_held&&ui_x==10&&ui_y==20); /* held finger survives idle */
    assert(stronghold_touch_ui_redraw()&&stronghold_touch_ui_help(0)==1);
    assert(MEM32(redraw)==2&&MEM32(help)==0); /* getters never mutate original requests/config */
    atomic_store(&enabled,0);
    assert(!stronghold_touch_ui_redraw()&&stronghold_touch_ui_help(0)==0&&stronghold_touch_ui_help(7)==7);
    atomic_store(&enabled,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_HOLD_END,80,10,20,0,0,1);
    stronghold_touch_frame();assert(!ui_held&&ui_x==-1&&ui_y==-1);
    assert(!stronghold_touch_ui_redraw()&&stronghold_touch_ui_help(0)==0&&stronghold_touch_ui_help(7)==7);
    assert(button_down==down&&button_up==up);
    stronghold_touch_enqueue(SH_TOUCH_UI_HOLD_BEGIN,80,10,20,0,0,1);
    stronghold_touch_frame();stronghold_touch_abort();stronghold_touch_frame();
    assert(button_down==down&&button_up==up&&!ui_held);
    assert(!stronghold_touch_ui_redraw()&&stronghold_touch_ui_help(0)==0);
    /* QUERY owns a live finger from the first frame, before the 600ms help
     * threshold. Repeated idle frames must neither drop hover nor activate. */
    stronghold_touch_enqueue(SH_TOUCH_QUERY,81,10,20,0,0,1);
    stronghold_touch_frame();assert(ui_tracking&&ui_world_guard&&ui_x==10&&ui_y==20);
    for(int frame=0;frame<8;frame++) {
        stronghold_touch_frame();assert(ui_tracking&&!ui_held&&ui_x==10&&ui_y==20);
        assert(stronghold_touch_ui_button(1)==0);
        assert(stronghold_touch_callback(CALLBACK_CONTEXT)==1);
        assert(stronghold_touch_callback(CALLBACK_INSPECT)==1);
    }
    assert(button_down==down&&button_up==up);
    stronghold_touch_enqueue(SH_TOUCH_UI_HOVER,81,12,22,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_HOVER,81,14,24,0,0,1);
    assert(queue_count==1);stronghold_touch_frame();
    assert(ui_tracking&&ui_x==14&&ui_y==24&&button_down==down&&button_up==up);
    stronghold_touch_enqueue(SH_TOUCH_UI_HOLD_END,81,10,20,0,0,1);
    stronghold_touch_frame();assert(!ui_tracking&&ui_x==-1&&ui_y==-1);
    stronghold_touch_frame();assert(!ui_world_guard);
    stronghold_touch_enqueue(SH_TOUCH_QUERY,81,10,20,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_CLICK,81,10,20,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_QUERY,82,30,40,0,0,1);
    stronghold_touch_frame();assert(button_down==down+1&&ui_x==10&&ui_y==20);
    assert(!ui_hover_guard&&stronghold_touch_ui_button(1)==1);
    /* Original440450/440660 is a separate type-0 world inspection callback.
     * It must see no desktop edge while the actual UI sees its click. */
    assert(stronghold_touch_legacy(1)==0);
    assert(ui_world_guard&&stronghold_touch_callback(CALLBACK_INSPECT)==1);
    assert(sequence==81&&stronghold_touch_status(82)==SH_TOUCH_PENDING);
    stronghold_touch_frame();assert(button_up==up+1&&ui_x==10&&ui_y==20);
    assert(ui_world_guard&&stronghold_touch_callback(CALLBACK_CONTEXT)==1);
    stronghold_touch_frame();assert(ui_x==10&&ui_y==20);
    assert(ui_world_guard&&stronghold_touch_callback(CALLBACK_INSPECT)==1); /* polled release frame */
    assert(stronghold_touch_legacy(1)==0);
    atomic_store(&enabled,0);assert(stronghold_touch_legacy(7)==7);atomic_store(&enabled,1);
    stronghold_touch_frame();assert(sequence==82&&ui_x==30&&ui_y==40);
    assert(stronghold_touch_status(82)==SH_TOUCH_UI);
    stronghold_touch_frame();assert(ui_tracking&&ui_x==30&&ui_y==40&&ui_hover_guard);
    /* No following QUERY is required to retire a completed click's point. */
    stronghold_touch_enqueue(SH_TOUCH_UI_CLICK,82,30,40,0,0,1);
    stronghold_touch_frame();assert(ui_x==30&&ui_y==40&&ui_release&&ui_drain==2);
    stronghold_touch_frame();assert(ui_x==30&&ui_y==40&&!ui_release&&ui_drain==1);
    stronghold_touch_frame();assert(ui_x==30&&ui_y==40&&ui_drain==0);
    stronghold_touch_frame();assert(ui_x==-1&&ui_y==-1&&ui_hover_guard);
    /* Original chooser normalized a rotated variant; repeating that exact
     * descriptor cancels the result, without duplicating the normalization. */
    MEM32(GAMESTATE)=14;
    const uint32_t roots[]={A(0x2598E60,0x30D6200),A(0x179FB28,0x20D2B50),A(0x2125920,0x2BB8E00)};
    for(unsigned i=0;i<3;i++)MEM32(roots[i]+44)=UINT32_MAX;
    uint32_t owner=0x3300000,node=0x3300500;
    memset(GPTR(owner),0,80);memset(GPTR(node),0,160);
    MEM32(UI_ROOT)=owner;MEM32(owner)=node;MEM32(node)=3;
    MEM32(node+4)=390;MEM32(node+8)=630;MEM32(node+12)=40;MEM32(node+16)=40;
    MEM32(node+20)=BUILD_SELECT;MEM32(node+24)=140;MEM32(node+76)=owner;MEM32(node+80)=102;
    MEM32(BUILD)=0;ui_drain=ui_release=ui_held=ui_tracking=ui_world_guard=0;
    down=button_down;cancels=0;MEM32(MAP)=0;
    stronghold_touch_enqueue(SH_TOUCH_QUERY,83,400,640,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_CLICK,83,400,640,0,0,1);
    stronghold_touch_frame();assert(button_down==down+1&&build_click_pending);
    MEM32(BUILD)=142; /* Real4447D0/444590 result, proven by original source. */
    stronghold_touch_frame();stronghold_touch_frame();stronghold_touch_frame();
    assert(last_build_node==node&&last_build_type==142);
    /* The armed button keeps the original name/cost tooltip after release,
     * without creating a mouse edge or changing the HELP preference. */
    MEM16(node+50)=0;stronghold_touch_after_world();
    assert(highlight[0]==390&&highlight[1]==630&&highlight[2]==40&&highlight[3]==40);
    assert(MEM16(node+50)==1&&lit_build_node==node&&MEM32(owner+56)==node);
    assert(stronghold_touch_ui_redraw()&&stronghold_touch_ui_redraw());
    assert(stronghold_touch_ui_help(0)==1&&MEM32(help)==0);
    MEM16(node+50)=0;stronghold_touch_after_world(); /* every input pass clears it */
    assert(MEM16(node+50)==1&&stronghold_touch_ui_redraw());
    MEM32(owner+56)=node+80;ui_tracking=1;stronghold_touch_after_world();
    assert(MEM32(owner+56)==node+80&&stronghold_touch_ui_help(0)==0); /* a live UI finger wins */
    ui_tracking=0;stronghold_touch_after_world();
    assert(MEM32(owner+56)==node&&stronghold_touch_ui_help(0)==1);
    MEM32(HUD_ANIMATION+24)=1;stronghold_touch_after_world(); /* not during a panel slide */
    assert(!highlight[2]&&!MEM16(node+50)&&!lit_build_node&&stronghold_touch_ui_redraw());
    MEM32(HUD_ANIMATION+24)=0;stronghold_touch_after_world();
    assert(MEM16(node+50)==1&&stronghold_touch_ui_redraw());
    stronghold_touch_enqueue(SH_TOUCH_QUERY,84,400,640,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_CLICK,84,400,640,0,0,1);
    stronghold_touch_frame();assert(action==SH_TOUCH_CANCEL&&button_down==down+1);
    stronghold_touch_after_world();assert(cancels==1&&!MEM32(BUILD));
    assert(stronghold_touch_ui_help(0)==0);
    /* Two-finger CANCEL is independent of the stale original mouse picker. */
    classification=SH_TOUCH_BUILDING;MEM32(BUILD)=25;MEM32(MAP)=0;
    stronghold_touch_enqueue(SH_TOUCH_CANCEL,84,200,200,0,0,1);
    stronghold_touch_frame();stronghold_touch_after_world();
    assert(cancels==2&&!MEM32(BUILD)&&button_down==down+1);
    classification=SH_TOUCH_UI;MEM32(MODE)=0;MEM32(BUILD)=25;
    stronghold_touch_enqueue(SH_TOUCH_CANCEL,84,400,640,0,0,1);
    stronghold_touch_frame();stronghold_touch_after_world();assert(cancels==3&&!MEM32(BUILD));
    uint32_t selected=A(0x11639A8,0x1163E08);MEM32(selected)=123;
    stronghold_touch_enqueue(SH_TOUCH_CANCEL,84,400,640,0,0,1);
    stronghold_touch_frame();stronghold_touch_after_world();assert(cancels==3&&MEM32(selected)==123);
    /* Replace the suppressed original inspection mouse edge with an explicit
     * native world decision. UI click/cancel, pan and OS abort keep the pane. */
    const int inspection_actions[]={SH_TOUCH_TAP,SH_TOUCH_DOUBLE_TAP,SH_TOUCH_CANCEL,
                                    SH_TOUCH_UI_CLICK,SH_TOUCH_PAN,SH_TOUCH_ABORT};
    for(unsigned i=0;i<sizeof inspection_actions/sizeof *inspection_actions;i++) {
        fixture(3);MEM32(GAMESTATE)=16;inspection_closes=0;
        MEM32(UI_ROOT)=owner;MEM32(owner)=node;MEM32(node)=0;
        MEM32(node+20)=A(0x440450,0x440660);
        classification=i==3?SH_TOUCH_UI:SH_TOUCH_WORLD;
        action=inspection_actions[i];aim_known=i<3||i==4;
        const unsigned offsets[]={0x10,0x14,0x28,0x30,0x34,0x3C,0x40,0x48};
        for(unsigned j=0;j<sizeof offsets/sizeof *offsets;j++)MEM32(INPUT+offsets[j])=0x12340000+j;
        stronghold_touch_after_world();
        assert(inspection_closes==(i<3));
        for(unsigned j=0;j<sizeof offsets/sizeof *offsets;j++)assert(MEM32(INPUT+offsets[j])==0x12340000+j);
    }
    fixture(3);MEM32(GAMESTATE)=16;classification=SH_TOUCH_UI;action=SH_TOUCH_CANCEL;
    MEM32(BUILD)=25;inspection_closes=0;stronghold_touch_after_world();assert(!inspection_closes);
    fixture(3);MEM32(GAMESTATE)=16;inspection_closes=0;
    MEM32(node+20)=CONTEXT;stronghold_touch_after_world();assert(!inspection_closes);
    MEM32(UI_ROOT)=0;
    uint32_t popup=A(0x2598E60,0x30D6200),popup_owner=0x3300000,popup_node=0x3300600;
    unsigned char saved_popup[128];memcpy(saved_popup,GPTR(popup),128);
    memset(GPTR(popup),0,128);memset(GPTR(popup_owner),0,80);memset(GPTR(popup_node),0,160);
    MEM32(GAMESTATE)=14;MEM32(popup+8)=30;MEM32(popup+12)=40;MEM32(popup+16)=204;MEM32(popup+20)=17;
    MEM32(popup+36)=popup_owner;MEM32(popup+44)=23;MEM32(popup_owner)=popup_node;
    MEM32(popup_owner+4)=180;MEM32(popup_owner+8)=56;
    MEM32(popup_node)=5;MEM32(popup_node+4)=2;MEM32(popup_node+8)=2;
    MEM32(popup_node+12)=200;MEM32(popup_node+16)=15;MEM32(popup_node+32)=20;
    MEM32(popup_node+20)=A(0x4AAD20,0x4AAEB0);MEM32(popup_node+76)=popup_owner;MEM32(popup_node+80)=102;
    assert(point(40,47)==SH_TOUCH_LIST&&ui_scroll_node==popup_node);
    scroll_min=0;scroll_max=10;scroll_current=0;ui_scroll_last=0;
    scroll_ui(60,0);assert(scroll_current==3);
    assert(point(190,60)==SH_TOUCH_UI&&ui_x==190&&ui_y==60);memcpy(GPTR(popup),saved_popup,128);
    /* Every native selection rebuilds its order group unfiltered after the queued clear. */
    fixture(0);resolving=1;selection_kind=1;assert(stronghold_touch_group_unfiltered());
    selection_kind=2;assert(stronghold_touch_group_unfiltered());
    selection_kind=0;assert(!stronghold_touch_group_unfiltered());
    selection_kind=1;resolving=0;assert(!stronghold_touch_group_unfiltered());selection_kind=0;
    /* Only native world evaluation/taps release the original owned-lord
     * single-player lock; construction, desktop and other gestures retain it. */
    fixture(0);resolving=1;assert(stronghold_touch_lord_control());
    action=-1;assert(stronghold_touch_lord_control());
    action=SH_TOUCH_DOUBLE_TAP;assert(!stronghold_touch_lord_control());
    action=SH_TOUCH_TAP;aim_known=0;assert(!stronghold_touch_lord_control());aim_known=1;
    MEM32(BUILD)=25;assert(!stronghold_touch_lord_control());MEM32(BUILD)=0;
    MEM32(MODE)=0;assert(!stronghold_touch_lord_control());MEM32(MODE)=1;
    atomic_store(&enabled,0);assert(!stronghold_touch_lord_control());atomic_store(&enabled,1);
    resolving=2;assert(!stronghold_touch_lord_control());resolving=0;
    /* A radar pair keeps first-finger ownership even if original hover opens a tooltip. */
    fixture(0);ui_drain=ui_release=ui_held=0;command_key=0;classification=SH_TOUCH_RADAR;
    stronghold_touch_enqueue(SH_TOUCH_ABORT,7,10,20,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_RADAR_TOGGLE,7,10,20,0,0,1);
    stronghold_touch_frame();assert(command_key=='R'&&command_modifiers==2);
    command_key=0;classification=SH_TOUCH_UI;
    stronghold_touch_enqueue(SH_TOUCH_RADAR_TOGGLE,7,10,20,0,0,1);
    stronghold_touch_frame();assert(!command_key);
    classification=SH_TOUCH_BOOK;tools_opened=0;
    stronghold_touch_enqueue(SH_TOUCH_OPEN_TOOLS,7,10,20,0,0,1);
    stronghold_touch_frame();assert(tools_opened==1);
    MEM32(GAMEOBJ+104)=1;ui_node=0;
    int ox=(int32_t)MEM32(A(0xF687E8,0xF68C48)),oy=(int32_t)MEM32(A(0xF687EC,0xF68C4C));
    assert(hud_class(SH_TOUCH_UI,ox+100,oy+490)==SH_TOUCH_BOOK);
    assert(hud_class(SH_TOUCH_UI,ox+740,oy+540)!=SH_TOUCH_BOOK);
    MEM32(GAMEOBJ+104)=0;assert(hud_class(SH_TOUCH_UI,ox+740,oy+540)==SH_TOUCH_BOOK);
    /* Tabs follow the HUD order and the original drawer's availability. */
    classification=SH_TOUCH_BUILD_PANEL;MEM32(GAMESTATE)=14;MEM32(GAMESTATE+4)=18;MEM32(GAMEOBJ+104)=2;keep_state=1;
    MEM32(HUD_ANIMATION+44)=0;feedback_bits=0;
    stronghold_touch_enqueue(SH_TOUCH_BUILD_SWIPE,7,10,20,100,0,1);
    stronghold_touch_frame();assert(MEM32(GAMESTATE+4)==20); /* a subsection's right swipe opens the section after its parent */
    assert(feedback_bits==SH_FEEDBACK_SECTION); /* the change is felt */
    assert(MEM32(HUD_ANIMATION+44)==1);          /* toggled by the original to -1: content slides right */
    stronghold_touch_enqueue(SH_TOUCH_BUILD_SWIPE,7,10,20,100,0,1);
    stronghold_touch_frame();assert(MEM32(GAMESTATE+4)==10); /* a section's right swipe opens the previous one */
    stronghold_touch_enqueue(SH_TOUCH_BUILD_SWIPE,7,10,20,100,0,1);
    stronghold_touch_frame();assert(MEM32(GAMESTATE+4)==25); /* wraps around; editor tabs skipped */
    stronghold_touch_enqueue(SH_TOUCH_BUILD_SWIPE,7,10,20,-100,0,1);
    stronghold_touch_frame();assert(MEM32(GAMESTATE+4)==10&&MEM32(HUD_ANIMATION+44)==UINT32_MAX); /* then +1: left */
    stronghold_touch_enqueue(SH_TOUCH_BUILD_SWIPE,7,10,20,-100,0,1);
    stronghold_touch_frame();assert(MEM32(GAMESTATE+4)==20);
    MEM32(GAMESTATE+4)=11;stronghold_touch_enqueue(SH_TOUCH_BUILD_SWIPE,7,10,20,-100,0,1);
    stronghold_touch_frame();assert(MEM32(GAMESTATE+4)==20); /* swipe left from subsection11 opens the next tab */
    MEM32(GAMEOBJ+104)=1;MEM32(GAMESTATE+4)=10;
    stronghold_touch_enqueue(SH_TOUCH_BUILD_SWIPE,7,10,20,100,0,1);
    stronghold_touch_frame();assert(MEM32(GAMESTATE+4)==29); /* the map editor keeps its own tabs */
    MEM32(GAMEOBJ+104)=2;keep_state=0;MEM32(GAMESTATE+4)=13;
    stronghold_touch_enqueue(SH_TOUCH_BUILD_SWIPE,7,10,20,-100,0,1);
    feedback_bits=0;stronghold_touch_frame();assert(MEM32(GAMESTATE+4)==13); /* before the keep nothing else opens */
    assert(!feedback_bits); /* no tick without a change */
    keep_state=1;MEM32(GAMEOBJ+104)=0;
    /* Long press: cancel only with something to cancel, otherwise original Space. */
    fixture(0);classification=SH_TOUCH_WORLD;MEM32(UNITS+32)=0;MEM32(UI_ROOT)=0;command_key=0;commands=0;
    stronghold_touch_enqueue(SH_TOUCH_HOLD,7,10,20,0,0,1);
    stronghold_touch_frame();assert(commands==1&&command_key==0x20&&command_modifiers==0&&action!=SH_TOUCH_CANCEL);
    MEM32(UNITS+32)=3;commands=0;
    stronghold_touch_enqueue(SH_TOUCH_HOLD,7,10,20,0,0,1);
    stronghold_touch_frame();assert(!commands&&action==SH_TOUCH_CANCEL);
    MEM32(UNITS+32)=0;MEM32(BUILD)=25;commands=0;
    stronghold_touch_enqueue(SH_TOUCH_HOLD,7,10,20,0,0,1);
    stronghold_touch_frame();assert(!commands&&action==SH_TOUCH_CANCEL);MEM32(BUILD)=0;
    classification=SH_TOUCH_RADAR;commands=0;
    stronghold_touch_enqueue(SH_TOUCH_HOLD,7,10,20,0,0,1);
    stronghold_touch_frame();assert(commands==1&&command_key==0x20);
    classification=SH_TOUCH_UI;commands=0;
    stronghold_touch_enqueue(SH_TOUCH_HOLD,7,10,20,0,0,1);
    stronghold_touch_frame();assert(!commands);
    /* Twists are discrete original camera turns; the indicator resets zoom. */
    classification=SH_TOUCH_WORLD;commands=0;
    stronghold_touch_enqueue(SH_TOUCH_ROTATE,7,10,20,1,0,1);
    stronghold_touch_enqueue(SH_TOUCH_ROTATE,7,10,20,-1,0,1);
    stronghold_touch_frame();assert(commands==2&&command_key==KEY_TURN_COUNTER);
    classification=SH_TOUCH_UI;commands=0;
    stronghold_touch_enqueue(SH_TOUCH_ROTATE,7,10,20,1,0,1);stronghold_touch_frame();assert(!commands);
    zoom_resets=0;stronghold_touch_enqueue(SH_TOUCH_ZOOM_RESET,0,0,0,0,0,1);stronghold_touch_frame();assert(zoom_resets==1);
    zoom_sessions=0;stronghold_touch_enqueue(SH_TOUCH_QUERY,8,10,20,0,0,1);stronghold_touch_frame();assert(zoom_sessions==1);
    mouse_checks();
    spawn_drag_checks();
    puts("native touch: tab swipes/availability/slide direction, hold relief-or-cancel, twist turns, zoom reset, unfiltered order groups, armed-button light, 17 list families/cdecl ABI, chat history/visibility, proportional scroll/coalescing/clamp/inverse, stale hover/touchpad, interaction priority, FIFO edges, hover-only HOLD/full-HUD description refresh without config writes, transparent UI first refusal PASS");
    return 0;
}
