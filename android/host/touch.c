/* Stronghold's native touch seam. UI retains its original input path. World
 * actions are evaluated after the original raster pick and enter the original
 * selection / contextual-order / placement code, without mouse button writes.
 * Address pairs below belong to the SHA-pinned Classic and Extreme images. */
#include "host.h"
#include "edition.h"
#include "input.h"
#include "display.h"
#include "view_controls.h"
#include "touch.h"
#include "game_ui.h"
#include "feedback.h"
#include <stdatomic.h>
#include <math.h>
#include <string.h>

#ifdef STRONGHOLD_CLASSIC
#define A(c, e) (c##u)
#else
#define A(c, e) (e##u)
#endif
#define INPUT A(0xF69DF8,0xF6A258)
#define MAP A(0x23443C0,0x2DD78A0)
#define UNITS A(0x14BEBC8,0x1593698)
#define MODE A(0x212587C,0x2BB8D5C)
#define BUILD A(0x212577C,0x2BB8C5C)
#define PLAYER A(0x1B6526C,0x25F874C)
#define GAMESTATE A(0x21259AC,0x2BB8E8C)
#define GAMEOBJ A(0x21259A0,0x2BB8E80)
#define CONTEXT A(0x434A70,0x434C80)
#define TERRAIN A(0x443BB0,0x443DF0)
#define PLACE A(0x445340,0x445580)
#define INSPECT A(0x4344F0,0x434700)
#define WALL A(0x438070,0x438280)
#define DEMOLISH A(0x438970,0x438B80)
#define PICK A(0x53E380,0x53E7D0)
#define CLEAR A(0x535B10,0x535F60)
#define CLEAR_GROUP A(0x537630,0x537A80)
#define SAME_TYPE A(0x535A70,0x535EC0)
#define UI_ROOT A(0xF687DC,0xF68C3C)
#define CHAT_BODY A(0x5EBD98,0x5EBC58)
#define CHAT_SCROLL A(0x4427B0,0x442980)
#define ADVANTAGE A(0x42A8C0,0x42A940)
#define BUILD_SELECT A(0x444590,0x4447D0)
#define SECTION_SELECT A(0x444D00,0x444F40)
#define HUD_ANIMATION A(0xF687D8,0xF68C38)
#define KEEP_STATE A(0x4597D0,0x459A20)
#define KEEP_OWNER A(0x1168500,0x1168960)
#define INSPECTION_OWNER A(0x440450,0x440660)
/* Original view keys: Space flattens relief; X/C turn the camera by 90 degrees. */
#define KEY_RELIEF 0x20
#define KEY_TURN_CLOCKWISE 'C'
#define KEY_TURN_COUNTER 'X'
#include "touch_lists.h"

enum { QUEUE_SIZE = 128, CALLBACK_CONTEXT = 0, CALLBACK_TERRAIN = 1,
       CALLBACK_PLACE = 2, CALLBACK_INSPECT = 3, CALLBACK_WALL=4, CALLBACK_DEMOLISH=5 };
typedef struct touch_event {
    int action;
    unsigned sequence;
    float x, y, dx, dy, scale;
} touch_event;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static touch_event queue[QUEUE_SIZE];
static unsigned queue_head, queue_count;
static _Atomic unsigned requested_sequence;
static _Atomic uint64_t published;
static _Atomic int enabled = 1, abort_requested;

/* Only the guest thread accesses everything below. */
static unsigned sequence;
static int classification = SH_TOUCH_BLOCKED, aim_known, aim_x, aim_y;
static int action = -1, resolving, commit_consumed, selection_kind;
static unsigned callbacks;
static int box_active, box_x, box_y, box_end_x, box_end_y;
static int ui_release, ui_held, ui_drain, ui_tracking, ui_world_guard, ui_dragging;
static float aim_nx, aim_ny;
static int ui_hit_active, ui_x, ui_y;
static int ui_hover_guard=1;
static uint32_t ui_node, last_build_node, last_build_type, lit_build_node;
static uint32_t slider_owner, slider_value;
static int build_click_pending, build_redraw, build_help;
static uint32_t ui_scroll_node, ui_scroll_last;
static const touch_list_family *ui_scroll_family;
static double ui_scroll_pixels;
static int active_callback, wall_anchor;
static int evaluated_context=4, evaluated_operation=-1;
/* Original WHEEL_DELTA units still owed to the game. Its input poll 4682B0
 * keeps only the last WM_MOUSEWHEEL of a frame, as one step: send one per frame. */
static int wheel_pending;

static void publish(int value)
{
    classification = value;
    uint64_t current=atomic_load_explicit(&published,memory_order_acquire);
    while ((unsigned)(current>>32)==sequence &&
           sequence==atomic_load_explicit(&requested_sequence,memory_order_acquire)) {
        uint64_t next=((uint64_t)sequence<<32)|(unsigned)value;
        if (atomic_compare_exchange_weak_explicit(&published,&current,next,
                                                  memory_order_release,memory_order_acquire)) break;
    }
}

static int continuous(int action)
{
    return action == SH_TOUCH_BOX_UPDATE || action == SH_TOUCH_AIM_UPDATE ||
           action == SH_TOUCH_PAN || action == SH_TOUCH_TRANSFORM || action == SH_TOUCH_UI_SCROLL ||
           action == SH_TOUCH_UI_HOVER || action == SH_TOUCH_WHEEL || action == SH_TOUCH_GRAB || action == SH_TOUCH_UI_DRAG_UPDATE;
}

static int accumulates(int action)
{
    return action == SH_TOUCH_PAN || action == SH_TOUCH_TRANSFORM || action == SH_TOUCH_UI_SCROLL ||
           action == SH_TOUCH_WHEEL || action == SH_TOUCH_GRAB;
}

void stronghold_touch_enqueue(int type, unsigned seq, float x, float y,
                             float dx, float dy, float scale)
{
    if (type < SH_TOUCH_QUERY || type > SH_TOUCH_UI_DRAG_END || !isfinite(x) ||
        !isfinite(y) || !isfinite(dx) || !isfinite(dy) || !isfinite(scale)) return;
    if (type == SH_TOUCH_QUERY) {
        atomic_store_explicit(&requested_sequence, seq, memory_order_release);
        atomic_store_explicit(&published, (uint64_t)seq << 32, memory_order_release);
    }
    pthread_mutex_lock(&queue_lock);
    if (queue_count && continuous(type)) {
        touch_event *last = &queue[(queue_head + queue_count - 1) % QUEUE_SIZE];
        if (last->action == type && last->sequence == seq) {
            if(type!=SH_TOUCH_UI_SCROLL){last->x = x; last->y = y;}
            if (accumulates(type)) {
                last->dx += dx; last->dy += dy;
                last->scale *= scale;
            } else { last->dx = dx; last->dy = dy; last->scale = scale; }
            pthread_mutex_unlock(&queue_lock);
            return;
        }
    }
    if (queue_count == QUEUE_SIZE) {
        /* Losing an edge cannot turn an interrupted gesture into a command. */
        queue_count = 0;
        atomic_store_explicit(&abort_requested, 1, memory_order_release);
        atomic_store_explicit(&published, ((uint64_t)seq << 32) | SH_TOUCH_BLOCKED,
                              memory_order_release);
        pthread_mutex_unlock(&queue_lock);
        return;
    }
    queue[(queue_head + queue_count++) % QUEUE_SIZE] = (touch_event){type, seq, x, y, dx, dy, scale};
    pthread_mutex_unlock(&queue_lock);
}

int stronghold_touch_status(unsigned seq)
{
    uint64_t status = atomic_load_explicit(&published, memory_order_acquire);
    return (unsigned)(status >> 32) == seq ? (int)(unsigned)status : SH_TOUCH_BLOCKED;
}

int stronghold_touch_desktop(void) { return !atomic_load_explicit(&enabled, memory_order_relaxed); }

void stronghold_touch_abort(void)
{
    /* Called by Android onPause: no guest memory, calls, or machine lock. */
    atomic_store_explicit(&abort_requested, 1, memory_order_release);
}

static void abort_gesture(void)
{
    aim_known = box_active = selection_kind = 0;
    wall_anchor=0;
    ui_hit_active = 1; ui_x = ui_y = -1;
    ui_scroll_node = ui_scroll_last = 0; ui_scroll_family = NULL; ui_scroll_pixels = 0;
    slider_owner=slider_value=0;
    action = -1;
    ui_hover_guard=1;
    ui_tracking=ui_world_guard=0;
    if (ui_release || ui_dragging) {
        if(ui_dragging)MEM32(A(0xB975C0,0xB97740))=UINT32_MAX;
        void_input_mouse_button(0, 0);
        /* An OS abort is not a release over a type-10 UI button. */
        ui_hit_active=1;ui_x=ui_y=-1;ui_drain=2;ui_world_guard=1;
    }
    ui_held = ui_release = ui_dragging = 0;
}

static int in_game(void)
{
    if (!void_vm_readable(MAP, 0xB4) || !void_vm_readable(GAMESTATE, 4)) return 0;
    uint32_t state = MEM32(GAMESTATE);
    /* MAP[0] is cursor-in-world, written by 4E9180; a desktop cursor left
     * over the HUD must not prevent a fresh native world query. */
    return state == 12 || state == 14 || state == 16;
}

static int world_blocked(void)
{
    return MEM32(A(0x212889C,0x2BBBD7C)) || MEM32(A(0x1A55F30,0x248CF80)) ||
           MEM32(A(0x1A56054,0x248D0A4));
}

/* Calls from host seams preserve the complete lifted register/flag file. */
static uint32_t call(uint32_t function, uint32_t self, int nargs, const uint32_t *args)
{
    void_regs saved;
    void_regs_save(&saved);
    uint32_t result = void_call_guest_this(function, self, nargs, args);
    void_regs_load(&saved);
    return result;
}

static uint32_t call_cdecl(uint32_t function, int nargs, const uint32_t *args)
{
    void_regs saved;
    void_regs_save(&saved);
    uint32_t result = void_call_guest(function, nargs, args, 0);
    void_regs_load(&saved);
    return result;
}

static int inspection_open(void)
{
    /* This active original owner starts with the separate inspection-world
     * callback440450/440660. */
    if(!void_vm_readable(UI_ROOT,4))return 0;
    uint32_t owner=MEM32(UI_ROOT);
    if(!owner||!void_vm_readable(owner,4))return 0;
    uint32_t node=MEM32(owner);
    return void_vm_readable(node,24)&&MEM32(node)==0&&MEM32(node+20)==INSPECTION_OWNER;
}

static void close_world_inspection(void)
{
    /* The inspection owner's desktop Ldown branch requests UI14; direct touch
     * replaces that edge only for a deliberate world tap or cancel. */
    if(!inspection_open())return;
    const uint32_t args[]={14,100};
    call(A(0x46B430,0x46B610),GAMEOBJ,2,args);
}

static int contains(int x, int y, int left, int top, int width, int height)
{
    return width > 0 && height > 0 && x >= left && y >= top &&
           (int64_t)x < (int64_t)left + width && (int64_t)y < (int64_t)top + height;
}

static int list_class(uint32_t node)
{
    uint32_t type=MEM32(node)&0x7FFFFFu;
    if(MEM32(node+20)==ADVANTAGE&&((MEM32(node+24)>=10&&MEM32(node+24)<=14)||MEM32(node+24)==4))return SH_TOUCH_DRAG;
    uint32_t dialog=MEM32(A(0x212594C,0x2BB8E2C));
    if(node==CHAT_BODY && MEM32(node+28)==A(0x42AD40,0x42ADC0) &&
       MEM32(node+24)==UINT32_MAX-2 && MEM32(A(0x1A559B0,0x248CA00))!=99 &&
       (dialog==UINT32_MAX||dialog==42||dialog==43||dialog==118||dialog==119||dialog==120)) {
        ui_scroll_node=node;ui_scroll_family=NULL;return SH_TOUCH_LIST;
    }
    if(type==5||type==6||MEM32(node+36)==2) {
        ui_scroll_node=node;ui_scroll_family=NULL;return SH_TOUCH_LIST;
    }
    for(unsigned i=0;i<sizeof touch_lists/sizeof *touch_lists;i++) {
        const touch_list_family *family=&touch_lists[i];
        if(node<family->first||node>family->last||(node-family->first)%80)continue;
        uint32_t bar=family->bar;
        if(!void_vm_readable(bar,80)||MEM32(bar+76)!=MEM32(node+76)||
           MEM32(bar+20)!=family->callback||MEM16(bar+54)||MEM16(bar+56))continue;
        ui_scroll_node=bar;ui_scroll_family=family;return SH_TOUCH_LIST;
    }
    return SH_TOUCH_UI;
}

/* The same active widget records traversed by original 4F6FF0 phase 0. Query
 * reads geometry only: it cannot activate controls, play hover sounds or run
 * callbacks. Type 0 records are the per-frame battlefield handlers. */
typedef int (*widget_match)(uint32_t node, int left, int top, const void *context);
static uint32_t find_widget(uint32_t owner, widget_match match, const void *context,
                            int *found_left, int *found_top)
{
    if (!owner || !void_vm_readable(owner, 80)) return 0;
    uint32_t node = MEM32(owner), state = MEM32(GAMESTATE + 4);
    for (unsigned n = 0; n < 2048 && void_vm_readable(node, 80); ++n, node += 80) {
        uint32_t raw = MEM32(node), type = raw & 0x7FFFFFu;
        if (raw == 102 || raw == 103) break;
        if (raw == 100) {
            if (MEM32(node + 24) != state) node += 80 * MEM32(node + 32);
            continue;
        }
        if (raw == 101) { if (MEM32(node + 24) == state) break; continue; }
        if (raw == 8) {
            if (MEM32(UI_ROOT+20)) return 0;
            continue;
        }
        if (raw == 9) {
            if (MEM32(A(0x212594C,0x2BB8E2C)) != UINT32_MAX) return 0;
            continue;
        }
        if (raw == 0x21000000u) {
            uint32_t skip=MEM32(node+32);
            if (skip>2048) return 0;
            uint32_t branch=node+80*(skip+1);
            for (unsigned v=0;v<128&&void_vm_readable(branch,80)&&MEM32(branch)==0x11000000u;v++) {
                if (MEM32(branch+40)==MEM32(GAMESTATE+8)) { node=branch;break; }
                skip=MEM32(branch+32);
                if (skip>2048) return 0;
                branch+=80*(skip+1);
            }
            continue;
        }
        if (raw == 0x11000000u) {
            uint32_t skip=MEM32(node+32);
            if (skip>2048) return 0;
            node+=80*skip;continue;
        }
        if (raw == 7 || raw == 0x01000000u || (raw & 0x04000000u)) continue;
        if (raw & 0x08000000u) {
            uint32_t branch=node+80;
            for (unsigned v=0;v<128&&void_vm_readable(branch,80)&&(MEM32(branch)&0x04000000u);v++,branch+=80) {
                if (MEM32(branch+40)==MEM32(GAMESTATE+8)) { node=branch;raw=MEM32(node);break; }
            }
        }
        if (raw & 0x80000000u) continue;
        type=raw&0x7FFFFFu;
        if (!type || type > 10 || MEM16(node + 54) || MEM16(node + 56)) continue;
        uint32_t widget_owner = MEM32(node + 76);
        if (!void_vm_readable(widget_owner, 12)) continue;
        int left = (int32_t)MEM32(node + 4) + (int32_t)MEM32(widget_owner + 4);
        int top = (int32_t)MEM32(node + 8) + (int32_t)MEM32(widget_owner + 8);
        const uint32_t roots[]={A(0x2598E60,0x30D6200),A(0x179FB28,0x20D2B50),A(0x2125920,0x2BB8E00)};
        for(unsigned i=0;i<3;i++) {
            uint32_t root=roots[i];
            if(!void_vm_readable(root,48)||!MEM32(root+44)||MEM32(root+44)==UINT32_MAX||MEM32(root+36)!=widget_owner)continue;
            /* Modal owners contain raster offsets; touch and root geometry are frame coordinates. */
            left=(int32_t)MEM32(node+4)+(int32_t)MEM32(root+8);
            top=(int32_t)MEM32(node+8)+(int32_t)MEM32(root+12);
            break;
        }
        if (match(node, left, top, context)) {
            if (found_left) *found_left = left;
            if (found_top) *found_top = top;
            return node;
        }
    }
    return 0;
}

static int hit_match(uint32_t node, int left, int top, const void *context)
{
    const int *p = context;
    return contains(p[0], p[1], left, top, (int32_t)MEM32(node + 12), (int32_t)MEM32(node + 16));
}

static int radar_hit_match(uint32_t node, int left, int top, const void *context)
{
    return MEM32(node+20)==A(0x434410,0x434620)&&hit_match(node,left,top,context);
}

static int widget_class(uint32_t owner, int x, int y)
{
    const int p[2] = {x, y};
    uint32_t node = find_widget(owner, hit_match, p, NULL, NULL);
    if (!node) return 0;
    ui_node = node;
    return list_class(node);
}

static int spawn_map_match(uint32_t node, int left, int top, const void *context)
{
    (void)left;(void)top;(void)context;
    return MEM32(node+28)==A(0x4281B0,0x428230);
}

static int spawn_map_active(void)
{
    return !in_game()&&MEM32(A(0x1A55B28,0x248CB78))&&
           void_vm_readable(UI_ROOT,4)&&
           find_widget(MEM32(UI_ROOT),spawn_map_match,NULL,NULL,NULL);
}

static int spawn_map_control(void)
{
    uint32_t map=void_vm_readable(UI_ROOT,4)?
        find_widget(MEM32(UI_ROOT),spawn_map_match,NULL,NULL,NULL):0;
    /* Original setup has type3/type10 map controls over its zero-size draw
     * record. Only that active map owner's flag callback may hold a drag. */
    return map&&ui_node&&MEM32(ui_node+20)==CHAT_SCROLL&&
           MEM32(ui_node+76)==MEM32(map+76);
}

static int spawn_icon(int x,int y)
{
    if(!spawn_map_active())return -1;
    int ox=(int32_t)MEM32(A(0xFD57A0,0xFD5C00));
    int oy=(int32_t)MEM32(A(0xFD57A4,0xFD5C04));
    int nearest=-1;double best=HUGE_VAL;
    for(unsigned i=0;i<8;i++) {
        uint32_t slot=A(0x21282A4,0x2BBB784)+8*i;
        int sx=(int32_t)MEM32(slot),sy=(int32_t)MEM32(slot+4);
        int cx=ox+572+sx+12,cy=oy+3+sy+16;
        double dx=(double)x-cx,dy=(double)y-cy,d=dx*dx+dy*dy;
        if(sx>=0&&contains(x,y,cx-36,cy-49,72,99)&&d<best){nearest=(int)i;best=d;}
    }
    return nearest;
}

static void spawn_snap(int *x,int *y)
{
    int slot=spawn_icon(*x,*y);
    if(slot<0)return;
    *x=(int32_t)MEM32(A(0xFD57A0,0xFD5C00))+584+(int32_t)MEM32(A(0x21282A4,0x2BBB784)+8*slot);
    *y=(int32_t)MEM32(A(0xFD57A4,0xFD5C04))+19+(int32_t)MEM32(A(0x21282A8,0x2BBB788)+8*slot);
}

typedef struct { int x; int64_t distance; uint32_t value; } slider_hit;
static int slider_match(uint32_t node,int left,int top,const void *context)
{
    (void)top;slider_hit *hit=(slider_hit *)context;
    uint32_t value=MEM32(node+24);
    if(MEM32(node+20)!=ADVANTAGE||value<10||value>14)return 0;
    int64_t delta=(int64_t)hit->x-left-(int32_t)MEM32(node+12)/2;
    if(delta<0)delta=-delta;
    if(delta<hit->distance){hit->distance=delta;hit->value=value;}
    return 0;
}

static int ui_class(int x, int y)
{
    ui_node=0;
    const uint32_t roots[] = { A(0x2598E60,0x30D6200),
                             A(0x179FB28,0x20D2B50),
                             A(0x2125920,0x2BB8E00) };
    for (unsigned i = 0; i < sizeof roots / sizeof *roots; ++i) {
        uint32_t root = roots[i];
        if (!void_vm_readable(root, 0x7C) || MEM32(root + 0x2C) == UINT32_MAX) continue;
        int result = widget_class(MEM32(root + 0x24), x, y);
        if (result) return result;
        if (contains(x, y, MEM32(root + 8), MEM32(root + 12), MEM32(root + 16), MEM32(root + 20)))
            return SH_TOUCH_UI;
    }
    return void_vm_readable(UI_ROOT, 4) ? widget_class(MEM32(UI_ROOT), x, y) : 0;
}

static int hud_exclusion(int x,int y)
{
    uint32_t index=MEM32(A(0x2128940,0x2BBBE20));
    if (!index||index>11) return 0;
    uint32_t table=A(0xB47640,0xB477D0)+160*(index-1);
    int ox=MEM32(A(0xF687E8,0xF68C48)),oy=MEM32(A(0xF687EC,0xF68C4C));
    for (unsigned i=0;i<10;i++,table+=16) {
        int left=MEM32(table),top=MEM32(table+4),w=MEM32(table+8),h=MEM32(table+12);
        if (left==-1) break;
        /* Original4E9180 uses inclusive edges for these portrait cutouts. */
        if ((int64_t)x>=left+ox&&(int64_t)x<=(int64_t)left+ox+w&&
            (int64_t)y>=top+oy&&(int64_t)y<=(int64_t)top+oy+h) return 1;
    }
    return 0;
}

/* The top-level HUD tab owning a building panel, through nested subsections. */
static int building_parent(int section)
{
    switch(section){case 11:case 12:case 13:case 14:case 18:case 19:return 10;
    case 15:case 33:return 30;default:return section;}
}

/* Original tab records, left to right on the HUD (identical in both PEs). */
static const int building_tabs[]={10,20,40,30,28,25,26,27,29};

/* Original tab drawer4645E0/464820 disables the map-editor tabs 26/27/29
 * outside editor modes 1/6 and greys the others until the player's keep and
 * granary exist (4597D0/459A20 > 0); panels48/61 hide all of them. */
static int building_tab_open(int section,int economy)
{
    uint32_t mode=MEM32(GAMEOBJ+104);
    int editor=mode==1||mode==6;
    if(section==26||section==27||section==29)return editor;
    uint32_t panel=MEM32(GAMESTATE+4);
    if(MEM32(GAMESTATE)==14&&(panel==61||(!editor&&panel==48)))return 0;
    return editor||economy;
}

static void swipe_building_tab(int step)
{
    int current=(int)MEM32(GAMESTATE+4),parent=building_parent(current),index=-1;
    for(int i=0;i<9;i++)if(building_tabs[i]==parent)index=i;
    if(index<0)return;
    int economy=(int32_t)call(KEEP_STATE,KEEP_OWNER,0,NULL)>0;
    /* From a subsection either swipe goes on to the section after its parent
     * tab instead of back to the parent; the slide still follows the finger. */
    int order=parent!=current?1:step;
    for(int n=1;n<9;n++) {
        int next=building_tabs[(index+order*n+81)%9];
        if(!building_tab_open(next,economy))continue;
        /* Original 4F5630/4F59F0 negates HUD+44 before each panel slide, and
         * 4F5680/4F5A40 move the content left while it is positive. Prime it
         * so the panel follows the finger: the next tab enters from the right. */
        if(void_vm_writable(HUD_ANIMATION+44,4))MEM32(HUD_ANIMATION+44)=step>0?UINT32_MAX:1u;
        uint32_t n32=(uint32_t)next;call_cdecl(SECTION_SELECT,1,&n32);
        stronghold_feedback(SH_FEEDBACK_SECTION);
        return;
    }
}

static int build_match(uint32_t node,int left,int top,const void *context)
{
    (void)left;(void)top;
    uint32_t building=*(const uint32_t *)context;
    if(MEM32(node+20)!=BUILD_SELECT||(int32_t)MEM32(node+12)<=0||(int32_t)MEM32(node+16)<=0)return 0;
    return (node==last_build_node&&last_build_type==building)||MEM32(node+24)==building;
}

/* Light the original construction button of the armed building with its own
 * hover frame: every original input pass (4F4C40/4F5000) clears record word
 * +50 and sets it under the cursor, and the draw pass (4F5380/4F5740) then
 * picks the lit frame. Runs between the two passes. The changed button needs
 * one full HUD pass; a sliding panel has a different drawing offset, so
 * nothing is lit during the slide. */
static void update_build_highlight(void)
{
    int left=0,top=0,width=0,height=0;
    uint32_t node=0,building=void_vm_readable(BUILD,4)?MEM32(BUILD):0;
    if(building&&in_game()&&MEM32(GAMESTATE)==14&&void_vm_readable(HUD_ANIMATION+24,4)&&
       !MEM32(HUD_ANIMATION+24)&&void_vm_readable(UI_ROOT,4)) {
        node=find_widget(MEM32(UI_ROOT),build_match,&building,&left,&top);
        if(node){width=(int32_t)MEM32(node+12);height=(int32_t)MEM32(node+16);MEM16(node+50)=1;}
    }
    if(node!=lit_build_node) {
        if(lit_build_node&&void_vm_writable(lit_build_node+50,2))MEM16(lit_build_node+50)=0;
        lit_build_node=node;build_redraw=1;
    }
    build_help=0;
    if(node&&atomic_load_explicit(&enabled,memory_order_relaxed)&&!ui_tracking&&!ui_held) {
        uint32_t owner=MEM32(node+76);
        if(void_vm_writable(owner+56,4)) {
            /* Original tooltip4F6E50/4F7210 reads the owner's hovered record. */
            MEM32(owner+56)=node;build_help=1;
        }
    }
    stronghold_display_highlight(left,top,width,height);
}

/* A stationary long press only cancels when there is something to cancel. */
static int cancellable(void)
{
    return MEM32(BUILD)||MEM32(MODE)==4||MEM32(MODE)==5||(int32_t)MEM32(UNITS+32)>0||inspection_open();
}
static int hud_class(int cls,int x,int y)
{
    int ox=(int32_t)MEM32(A(0xF687E8,0xF68C48)),oy=(int32_t)MEM32(A(0xF687EC,0xF68C4C));
    int editor=MEM32(GAMEOBJ+104)==1;
    if(ui_node&&MEM32(ui_node+20)==A(0x434410,0x434620))return SH_TOUCH_RADAR;
    if(editor?contains(x,y,ox+78,oy+475,44,50):contains(x,y,ox+686,oy+503,110,97))return SH_TOUCH_BOOK;
    int section=building_parent((int)MEM32(GAMESTATE+4));
    if(MEM32(GAMESTATE)==14&&contains(x,y,ox,oy+472,510,128)&&
       ((section>=10&&section<=14)||section==20||(section>=25&&section<=30)||section==40)) {
        if(cls!=SH_TOUCH_LIST)return SH_TOUCH_BUILD_PANEL;
    }
    return cls;
}
static int ui_classification(int cls)
{return cls==SH_TOUCH_UI||cls==SH_TOUCH_LIST||cls==SH_TOUCH_BOOK||cls==SH_TOUCH_RADAR||cls==SH_TOUCH_BUILD_PANEL||cls==SH_TOUCH_DRAG||cls==SH_TOUCH_MENU;}

static int point(float nx, float ny)
{
    ui_node=ui_scroll_node=0;ui_scroll_family=NULL;
    int x, y, layer = stronghold_display_map_point(nx, ny, &x, &y);
    if (!layer) return SH_TOUCH_BLOCKED;
    ui_hit_active = 1;
    if (!in_game()) {
        aim_x = ui_x = x; aim_y = ui_y = y;
        int cls = ui_class(x,y);
        if((!cls||spawn_map_control())&&spawn_icon(x,y)>=0)return SH_TOUCH_DRAG;
        return cls ? cls : SH_TOUCH_UI;
    }
    const uint32_t roots[] = { A(0x2598E60,0x30D6200), A(0x179FB28,0x20D2B50),
                              A(0x2125920,0x2BB8E00) };
    const int groups[] = {2,1,2};
    for (unsigned i=0;i<3;i++) {
        uint32_t root=roots[i]; int qx,qy;
        if (!void_vm_readable(root,0x7C) || MEM32(root+0x2C)==UINT32_MAX ||
            !MEM32(root+16) || !MEM32(root+20) ||
            !stronghold_display_ui_point(MEM32(root+0x2C)&&MEM32(root+0x2C)!=19?groups[i]:0,nx,ny,&qx,&qy)) continue;
        int cls=widget_class(MEM32(root+0x24),qx,qy);
        if (!cls && i<2 && contains(qx,qy,MEM32(root+8),MEM32(root+12),MEM32(root+16),MEM32(root+20)))
            cls=SH_TOUCH_UI;
        if (i==2&&MEM32(root+44)==5)cls=SH_TOUCH_MENU;
        if (cls) { aim_x=ui_x=qx;aim_y=ui_y=qy;return cls; }
        if (i==0) {
            /* The original value-popup handler consumes outside clicks to close its dropdown. */
            if(MEM32(root+44)==23){aim_x=ui_x=qx;aim_y=ui_y=qy;return SH_TOUCH_UI;}
            return SH_TOUCH_BLOCKED;
        }
    }
    int hx,hy;
    if (stronghold_display_ui_point(0,nx,ny,&hx,&hy)) {
        const int p[]={hx,hy};
        /* Generic HUD records overlap the radar; its entire active rectangle owns a swipe. */
        ui_node=void_vm_readable(UI_ROOT,4)?find_widget(MEM32(UI_ROOT),radar_hit_match,p,NULL,NULL):0;
        int cls=ui_node?SH_TOUCH_UI:void_vm_readable(UI_ROOT,4)?widget_class(MEM32(UI_ROOT),hx,hy):0;
        uint32_t state=MEM32(GAMESTATE), substate=MEM32(GAMESTATE+4);
        int full=(state==12||state==14)&&(substate==60||substate==62);
        if (!cls&&((!full && contains(hx,hy,0,(int)stronghold_display_height(720)-128,
                              stronghold_display_width(1280),128)) || hud_exclusion(hx,hy))) cls=SH_TOUCH_UI;
        if (cls) { aim_x=ui_x=hx;aim_y=ui_y=hy;return hud_class(cls,hx,hy); }
    }
    if (layer == 2) {
        /* Pixel occupancy is only the final background hint, after every
         * actual panel/widget rectangle with its own inverse transform. */
        aim_x=ui_x=x;aim_y=ui_y=y;return SH_TOUCH_UI;
    }
    if (!stronghold_display_world_point(nx,ny,&x,&y)) return SH_TOUCH_BLOCKED;
    ui_x=ui_y=-1;
    aim_x = x; aim_y = y;
    if (world_blocked()) return SH_TOUCH_BLOCKED;
    if (MEM32(BUILD)) return SH_TOUCH_BUILDING;
    if (MEM32(MODE) == 4 || MEM32(MODE) == 5) return SH_TOUCH_ABILITY;
    return SH_TOUCH_WORLD;
}

/* Whether a desktop cursor is over the match world: the touch classifier,
 * without disturbing the hover/aim state of a live touch. */
static int cursor_on_world(float nx, float ny)
{
    if (!in_game()) return 0;
    uint32_t node=ui_node,scroll=ui_scroll_node;
    const touch_list_family *family=ui_scroll_family;
    int hit=ui_hit_active,x=ui_x,y=ui_y,ax=aim_x,ay=aim_y;
    int cls=point(nx,ny);
    ui_node=node;ui_scroll_node=scroll;ui_scroll_family=family;
    ui_hit_active=hit;ui_x=x;ui_y=y;aim_x=ax;aim_y=ay;
    return cls>=SH_TOUCH_WORLD&&cls<=SH_TOUCH_BUILDING;
}

static void rectangle(int x1, int y1, int x2, int y2)
{
    /* Exact 4687D0 / 468880 convention: original unit hit boxes are in the
     * scrolling offscreen surface. Native zoom doubles both endpoints. */
    int zoom = MEM32(MAP + 0x90) ? 2 : 1;
    int ox = MEM32(MAP + 0x80), oy = MEM32(MAP + 0x84);
    MEM32(INPUT + 0x60) = zoom * (x1 + ox);
    MEM32(INPUT + 0x64) = zoom * (y1 + oy);
    MEM32(INPUT + 0x68) = zoom * (x2 + ox);
    MEM32(INPUT + 0x6C) = zoom * (y2 + oy);
    MEM32(INPUT + 0x70) = x1 + ox;
    MEM32(INPUT + 0x74) = y1 + oy;
    MEM32(INPUT + 0x78) = x2 + ox;
    MEM32(INPUT + 0x7C) = y2 + oy;
    MEM32(INPUT + 0x80) = zoom * (x1 + ox);
    MEM32(INPUT + 0x84) = zoom * (y1 + oy);
    MEM32(INPUT + 0x88) = zoom * (x2 + ox);
    MEM32(INPUT + 0x8C) = zoom * (y2 + oy);
}

static void scroll_ui(float dx,float dy)
{
    uint32_t node=ui_scroll_node;
    if(!node||!void_vm_writable(node,80)||!MEM32(node+20))return;
    if(node!=ui_scroll_last){ui_scroll_pixels=0;ui_scroll_last=node;}
    uint32_t type=MEM32(node)&0x7FFFFFu;
    double delta=type==5?stronghold_display_ui_dx(dx):stronghold_display_ui_dy(dy);
    if(!isfinite(delta))return;
    if(node==CHAT_BODY) {
        /* History is reverse chronological: dragging content downward reveals
         * older messages. The original callback owns both guards and 0..15. */
        ui_scroll_pixels+=delta;
        double whole=trunc(ui_scroll_pixels/17+copysign(1e-6,ui_scroll_pixels));
        int steps=(int)fmax(-15,fmin(15,whole));
        uint32_t argument=steps>0?UINT32_MAX:UINT32_MAX-1;
        for(int i=0;i<abs(steps);i++)call_cdecl(CHAT_SCROLL,1,&argument);
        ui_scroll_pixels-=whole*17;
        if(fabs(ui_scroll_pixels)<.00017)ui_scroll_pixels=0;
        return;
    }
    if(type!=5&&type!=6) {
        /* Original wheel-only descriptors call callback(direction), not the
         * five-argument scrollbar ABI. Avoid its ten-calls-per-frame loop. */
        ui_scroll_pixels-=delta;
        int height=(int32_t)MEM32(node+16);
        double pixels=height>0&&height<=64?height:20;
        double whole=trunc(ui_scroll_pixels/pixels+copysign(1e-6,ui_scroll_pixels));
        int steps=(int)fmax(-64,fmin(64,whole));
        uint32_t argument=steps>0?2:1;
        for(int i=0;i<abs(steps);i++)call_cdecl(MEM32(node+20),1,&argument);
        ui_scroll_pixels-=whole*pixels;
        if(fabs(ui_scroll_pixels)<pixels*1e-5)ui_scroll_pixels=0;
        return;
    }
    uint32_t args[]={MEM32(node+24),4,node+60,node+64,node+68};
    call_cdecl(MEM32(node+20),5,args); /* Refresh; event1 can reset the list. */
    int32_t minimum=(int32_t)MEM32(node+60),maximum=(int32_t)MEM32(node+64);
    if(maximum<minimum)maximum=minimum;
    int32_t current=(int32_t)MEM32(node+68);
    if(current<minimum)current=minimum;if(current>maximum)current=maximum;
    double range=(double)maximum-minimum,pixels,units=1;
    if(ui_scroll_family) {
        pixels=ui_scroll_family->pixels;units=ui_scroll_family->units;
        ui_scroll_pixels-=delta; /* The content follows the finger. */
    } else {
        int extent=(int32_t)MEM32(node+(type==5?12:16));
        if(range<=0||extent<=0){ui_scroll_pixels=0;return;}
        double thumb=fmax((int32_t)MEM32(node+32),floor(extent/(range+1)));
        pixels=fmax(1,extent-fmin(extent,thumb))/range;
        ui_scroll_pixels+=delta; /* A scrollbar thumb follows the finger. */
    }
    /* Motion is float-normalized. A sub-ULP residue at an exact row boundary
     * must not delay the next opposite step by a whole row. */
    double steps=trunc(ui_scroll_pixels/pixels+copysign(1e-6,ui_scroll_pixels));
    if(!steps)return;
    ui_scroll_pixels-=steps*pixels;
    if(fabs(ui_scroll_pixels)<pixels*1e-5)ui_scroll_pixels=0;
    int32_t next=(int32_t)fmax(minimum,fmin(maximum,current+steps*units));
    if(next==current)return;
    MEM32(node+64)=(uint32_t)maximum;MEM32(node+68)=(uint32_t)next;
    args[1]=3;call_cdecl(MEM32(node+20),5,args);
}

void stronghold_touch_frame(void)
{
    stronghold_game_ui_frame();
    if(!atomic_load_explicit(&enabled,memory_order_relaxed)&&void_vm_readable(INPUT,0x4c)&&MEM32(INPUT+0x34)){
        ui_node=0;ui_class((int32_t)MEM32(INPUT+0x10),(int32_t)MEM32(INPUT+0x14));
        stronghold_game_ui_click(ui_node);
    }
    callbacks = 0;
    action = -1;
    commit_consumed = selection_kind = 0;
    if (atomic_exchange_explicit(&abort_requested, 0, memory_order_acq_rel)) {
        pthread_mutex_lock(&queue_lock);
        unsigned kept=0;
        for (unsigned i=0;i<queue_count;i++) {
            touch_event e=queue[(queue_head+i)%QUEUE_SIZE];
            if (e.action==SH_TOUCH_MODE||e.action==SH_TOUCH_UI_SCALE)
                queue[(queue_head+kept++)%QUEUE_SIZE]=e;
        }
        queue_count=kept;
        pthread_mutex_unlock(&queue_lock);
        abort_gesture();
        wheel_pending=0;
        sequence=atomic_load_explicit(&requested_sequence,memory_order_acquire);
        publish(SH_TOUCH_BLOCKED);
    }
    if (wheel_pending>=120||wheel_pending<=-120) {
        int step=wheel_pending>0?120:-120;
        void_input_mouse_wheel(step);wheel_pending-=step;
    }
    /* Runs right after the original input poll set its edge-scroll flags
     * (right, left, bottom, top). Desktop edge scrolling is never meaningful
     * for a direct touch UI; a desktop pointer scrolls at the physical screen
     * edge, which the poll's own cursor misses under a world zoom or HUD scale. */
    if (void_vm_writable(A(0x11684C0,0x1168920), 16)) {
        int edges = atomic_load_explicit(&enabled, memory_order_relaxed) ? 0 : void_input_edges();
        if (edges >= 0) for (unsigned i = 0; i < 4; i++) MEM32(A(0x11684C0,0x1168920) + 4 * i) = (uint32_t)(edges >> i & 1);
    }
    if (ui_release) { void_input_mouse_button(0, 0); ui_release = 0; }
    if (ui_drain) { --ui_drain; return; }
    if(build_click_pending) {
        /* The original chooser may normalize rotated building variants. Save
         * its actual result after both original down/release frames, rather
         * than copying that engine algorithm into the touch layer. */
        last_build_type=MEM32(BUILD);build_click_pending=0;
    }
    /* QUERY starts hover immediately. Keep it through the physical finger's
     * lifetime, then retire it only after the original click/release drain.
     * UI ownership also covers the frame where polling observes LUp. */
    if(atomic_load_explicit(&enabled,memory_order_relaxed)&&!ui_tracking&&!ui_held) {
        ui_hit_active=1;ui_x=ui_y=-1;ui_hover_guard=1;ui_world_guard=0;
    }
    for (unsigned count = 0; count < QUEUE_SIZE; ++count) {
        touch_event event;
        pthread_mutex_lock(&queue_lock);
        if (!queue_count) { pthread_mutex_unlock(&queue_lock); break; }
        if (queue[queue_head].action==SH_TOUCH_QUERY && ui_held) {
            pthread_mutex_unlock(&queue_lock);
            abort_gesture();return;
        }
        event = queue[queue_head]; queue_head = (queue_head + 1) % QUEUE_SIZE; --queue_count;
        pthread_mutex_unlock(&queue_lock);
        if (event.action == SH_TOUCH_UI_SCALE) {
            stronghold_display_set_ui_scale(event.scale); continue;
        }
        if (event.action == SH_TOUCH_MODE) {
            abort_gesture();
            atomic_store_explicit(&enabled, event.scale != 0.0f, memory_order_relaxed);
            publish(SH_TOUCH_BLOCKED);
            continue;
        }
        if (event.action == SH_TOUCH_ZOOM_RESET) {
            /* An Android indicator tap, outside any game touch sequence. */
            stronghold_display_zoom_reset();
            continue;
        }
        if (event.action == SH_TOUCH_WHEEL) {
            /* Over the match world a mouse wheel zooms around the cursor (the
             * original in-match wheel only cycled map marks, 433700); menus,
             * lists and the HUD keep the original wheel. */
            if (cursor_on_world(event.x, event.y)) {
                if (event.dy) stronghold_display_zoom_session();
                stronghold_display_transform(event.x, event.y, 0, 0, event.scale);
            } else if (event.dx) {
                wheel_pending=(int)fmax(-1200,fmin(1200,(double)wheel_pending+event.dx));
            }
            continue;
        }
        if (event.action == SH_TOUCH_GRAB) {
            /* Middle-button drag: the map follows the mouse, as under a finger. */
            if (in_game()) stronghold_display_transform(event.x, event.y, event.dx, event.dy, 1.0f);
            continue;
        }
        if (!atomic_load_explicit(&enabled, memory_order_relaxed)) {
            if (event.action==SH_TOUCH_QUERY) { sequence=event.sequence;publish(SH_TOUCH_BLOCKED); }
            continue;
        }
        if (event.action == SH_TOUCH_QUERY) {
            abort_gesture(); sequence = event.sequence;
            stronghold_display_zoom_session();
            aim_nx = event.x; aim_ny = event.y;
            int cls = point(event.x, event.y);
            slider_owner=cls==SH_TOUCH_DRAG&&ui_node&&MEM32(ui_node+20)==ADVANTAGE?MEM32(ui_node+76):0;
            slider_value=0;
            aim_known = cls == SH_TOUCH_WORLD || cls == SH_TOUCH_ABILITY || cls == SH_TOUCH_BUILDING;
            ui_tracking=ui_world_guard=ui_classification(cls);
            publish(cls);
            continue;
        }
        if (event.sequence != sequence) continue;
        if (event.action == SH_TOUCH_ABORT) { abort_gesture(); continue; }
        if(event.action>=SH_TOUCH_UI_DRAG_BEGIN&&event.action<=SH_TOUCH_UI_DRAG_END) {
            int x,y;
            if(classification==SH_TOUCH_DRAG&&slider_owner) {
                uint32_t modal=MEM32(A(0x2598E60,0x30D6200)+44);
                if(!stronghold_display_map_point(event.x,event.y,&x,&y)||MEM32(UI_ROOT)!=slider_owner||
                   (modal&&modal!=UINT32_MAX)||
                   (ui_class(x,y)&&ui_node&&MEM32(ui_node+76)!=slider_owner)) {abort_gesture();break;}
                slider_hit hit={x,INT64_MAX,0};
                find_widget(slider_owner,slider_match,&hit,NULL,NULL);
                if(hit.value&&hit.value!=slider_value){call_cdecl(ADVANTAGE,1,&hit.value);slider_value=hit.value;}
                if(event.action==SH_TOUCH_UI_DRAG_END){abort_gesture();slider_owner=0;}
                break;
            }
            if(classification!=SH_TOUCH_DRAG||!spawn_map_active()||
               !stronghold_display_map_point(event.x,event.y,&x,&y)) {abort_gesture();break;}
            /* A modal appearing mid-drag must not receive its pending release. */
            if(ui_class(x,y)&&!spawn_map_control()){abort_gesture();break;}
            if(event.action!=SH_TOUCH_UI_DRAG_UPDATE)spawn_snap(&x,&y);
            ui_hit_active=ui_tracking=ui_world_guard=1;ui_hover_guard=0;
            ui_x=aim_x=x;ui_y=aim_y=y;void_input_mouse_warp(x,y);
            if(event.action==SH_TOUCH_UI_DRAG_BEGIN) {
                if(!ui_dragging){void_input_mouse_button(0,1);ui_dragging=1;}
            } else if(event.action==SH_TOUCH_UI_DRAG_END) {
                if(ui_dragging)void_input_mouse_button(0,0);
                ui_dragging=ui_tracking=0;ui_drain=2;
            }
            break;
        }
        if(event.action==SH_TOUCH_OPEN_TOOLS) {
            if(classification==SH_TOUCH_BOOK)stronghold_game_tools();abort_gesture();break;
        }
        if(event.action==SH_TOUCH_RADAR_TOGGLE) {
            /* The first finger owns the radar; an original hover tooltip may cover the pair's centroid. */
            if(classification==SH_TOUCH_RADAR&&in_game())stronghold_game_command('R',2);
            abort_gesture();break;
        }
        if(event.action==SH_TOUCH_BUILD_SWIPE) {
            /* Finger left reveals the tab on the right, as a page turns. */
            if(classification==SH_TOUCH_BUILD_PANEL&&in_game()&&!world_blocked()&&event.dx)
                swipe_building_tab(event.dx<0?1:-1);
            abort_gesture();break;
        }
        if(event.action==SH_TOUCH_HOLD) {
            if(in_game()&&classification==SH_TOUCH_RADAR)stronghold_game_command(KEY_RELIEF,0);
            else if(in_game()&&classification>=SH_TOUCH_WORLD&&classification<=SH_TOUCH_BUILDING) {
                if(cancellable()){abort_gesture();action=SH_TOUCH_CANCEL;break;}
                stronghold_game_command(KEY_RELIEF,0);
            }
            abort_gesture();break;
        }
        if(event.action==SH_TOUCH_ROTATE) {
            /* A rotation is a discrete original camera turn; the pair keeps panning. */
            if(in_game()&&event.dx&&classification>=SH_TOUCH_WORLD&&classification<=SH_TOUCH_BUILDING)
                stronghold_game_command(event.dx>0?KEY_TURN_CLOCKWISE:KEY_TURN_COUNTER,0);
            continue;
        }
        if (event.action == SH_TOUCH_PAN || event.action == SH_TOUCH_TRANSFORM) {
            abort_gesture();
            if ((classification >= SH_TOUCH_WORLD && classification <= SH_TOUCH_BUILDING)||
                (event.action==SH_TOUCH_PAN&&classification==SH_TOUCH_RADAR&&in_game()&&
                 MEM32(A(0x212594C,0x2BB8E2C))==UINT32_MAX&&!world_blocked()))
                stronghold_display_transform(event.x, event.y,
                    event.dx * (classification == SH_TOUCH_RADAR ? SH_RADAR_PAN_GAIN : 1.f),
                    event.dy * (classification == SH_TOUCH_RADAR ? SH_RADAR_PAN_GAIN : 1.f),
                    event.action == SH_TOUCH_TRANSFORM ? event.scale : 1.0f);
            continue;
        }
        if (event.action == SH_TOUCH_CANCEL) {
            /* A deliberate two-finger UI tap can dismiss an armed placement,
             * but must not clear a building/unit inspection behind a menu. */
            int ui=ui_classification(classification);
            int armed=MEM32(BUILD)||MEM32(MODE)==4||MEM32(MODE)==5;
            abort_gesture();if(!ui||armed)action=SH_TOUCH_CANCEL;break;
        }
        if (event.action == SH_TOUCH_UI_HOLD_END) {
            ui_held=ui_tracking=0;ui_hover_guard=1;ui_hit_active=1;ui_x=ui_y=-1;break;
        }
        int cls = point(event.x, event.y);
        if ((event.action >= SH_TOUCH_UI_CLICK && event.action <= SH_TOUCH_UI_SCROLL) ||
            event.action==SH_TOUCH_UI_HOVER) {
            if (!ui_classification(classification) || !ui_classification(cls)) { abort_gesture(); break; }
            aim_known = 0;
            ui_world_guard=1;
            int build_button=ui_node&&MEM32(ui_node+20)==BUILD_SELECT;
            if(event.action==SH_TOUCH_UI_CLICK&&classification==SH_TOUCH_MENU&&!ui_node) {
                uint32_t root=A(0x2125920,0x2BB8E00);
                int outside=!contains(ui_x,ui_y,MEM32(root+8),MEM32(root+12),MEM32(root+16),MEM32(root+20));
                abort_gesture();
                if(outside&&MEM32(root+44)==5)call(A(0x493F70,0x494100),A(0x11639F0,0x1163E50),0,NULL);
                break;
            }
            if(event.action==SH_TOUCH_UI_CLICK&&in_game()&&build_button&&MEM32(BUILD)&&
               ((last_build_node==ui_node&&last_build_type==MEM32(BUILD))||MEM32(ui_node+24)==MEM32(BUILD))) {
                /* Repeating the selected construction control is an explicit
                 * touch toggle. Cancel through the original right-cancel path,
                 * without sending another UI click or a battlefield edge. */
                abort_gesture();action=SH_TOUCH_CANCEL;break;
            }
            void_input_mouse_warp(aim_x, aim_y);
            if (event.action == SH_TOUCH_UI_CLICK) {
                stronghold_game_ui_click(ui_node);
                if(build_button){last_build_node=ui_node;build_click_pending=1;}
                ui_hover_guard=0;ui_tracking=ui_held=0;
                void_input_mouse_button(0, 1); ui_release = 1;ui_drain=2;
            } else if (event.action == SH_TOUCH_UI_HOLD_BEGIN) {
                /* A description hold is original hover, never a button edge. */
                ui_hover_guard=1;ui_held=ui_tracking=1;
            } else if (event.action == SH_TOUCH_UI_HOLD_END) {
                ui_held=ui_tracking=0;ui_hover_guard=1;ui_hit_active=1;ui_x=ui_y=-1;
            } else if (event.action == SH_TOUCH_UI_HOVER) {
                ui_hover_guard=1;ui_tracking=1;
            } else {
                ui_hover_guard=1;
                scroll_ui(event.dx,event.dy);
            }
            break;
        }
        if (classification < SH_TOUCH_WORLD || classification > SH_TOUCH_BUILDING ||
            cls < SH_TOUCH_WORLD || cls > SH_TOUCH_BUILDING) { aim_known = 0; continue; }
        aim_nx = event.x; aim_ny = event.y; aim_known = 1;
        action = event.action;
        if (action == SH_TOUCH_BOX_BEGIN) {
            box_active = 1; box_x = box_end_x = aim_x; box_y = box_end_y = aim_y;
        } else if (action == SH_TOUCH_BOX_UPDATE || action == SH_TOUCH_BOX_END) {
            box_end_x = aim_x; box_end_y = aim_y;
        }
        /* One semantic edge per rendered pick. Motion can coalesce, commits
         * cannot be consumed against an earlier point or a later sequence. */
        break;
    }
}

int stronghold_touch_callback(int kind)
{
    if (!atomic_load_explicit(&enabled, memory_order_relaxed) || resolving) return 0;
    callbacks |= 1u << kind;
    return ui_world_guard || aim_known || kind == CALLBACK_TERRAIN || kind == CALLBACK_PLACE ||
           kind==CALLBACK_WALL || kind==CALLBACK_DEMOLISH ||
           (kind == CALLBACK_CONTEXT && (MEM32(MODE) == 4 || MEM32(MODE) == 5));
}

uint32_t stronghold_touch_legacy(uint32_t original)
{
    return atomic_load_explicit(&enabled, memory_order_relaxed) ? 0 : original;
}
uint32_t stronghold_touch_x(uint32_t original) { return aim_known ? (uint32_t)aim_x : original; }
uint32_t stronghold_touch_y(uint32_t original) { return aim_known ? (uint32_t)aim_y : original; }
uint32_t stronghold_touch_ui_x(uint32_t original) { return atomic_load_explicit(&enabled,memory_order_relaxed)&&ui_hit_active ? (uint32_t)ui_x : original; }
uint32_t stronghold_touch_ui_y(uint32_t original) { return atomic_load_explicit(&enabled,memory_order_relaxed)&&ui_hit_active ? (uint32_t)ui_y : original; }
uint32_t stronghold_touch_ui_button(uint32_t original)
{
    return atomic_load_explicit(&enabled,memory_order_relaxed)&&ui_hover_guard?0:original;
}
static int ui_hold_redraw(void)
{
    /* Original HELP expires unless the full HUD refreshes its hovered
     * description; keep the armed button's name/cost as well as finger holds. */
    return atomic_load_explicit(&enabled,memory_order_relaxed)&&
           (ui_held||(build_help&&in_game()&&MEM32(BUILD)&&!world_blocked()));
}
int stronghold_touch_ui_redraw(void)
{
    /* A newly lit or unlit construction button is drawn by one full pass. */
    if(build_redraw){build_redraw=0;return 1;}
    return ui_hold_redraw();
}
uint32_t stronghold_touch_ui_help(uint32_t original)
{
    return ui_hold_redraw()?1:original;
}
int stronghold_touch_known(void) { return !atomic_load_explicit(&enabled, memory_order_relaxed) || aim_known; }
int stronghold_touch_selection(void) { return resolving ? selection_kind : 0; }
int stronghold_touch_group_unfiltered(void)
{
    /* Every native selection queues the original group clear (537630) in the
     * same frame. Until that command runs, filtered 5363F0 still sees the
     * previous group marks (unit+0x6C6) and skips those units: highlighted,
     * yet outside the new order group. The original double-click builds its
     * group with unfiltered 5364C0 right after its own clear; native taps,
     * double taps and boxes do the same. The lord stays as flagged. */
    return resolving && selection_kind;
}

int stronghold_touch_lord_control(void)
{
    /* Native tap/evaluation only; each original consumer retains its own
     * selection, ownership and route checks before accepting the order. */
    return atomic_load_explicit(&enabled,memory_order_relaxed)&&resolving==1&&aim_known&&
           MEM32(MODE)==1&&!MEM32(BUILD)&&(action==SH_TOUCH_TAP||action==-1);
}

int stronghold_touch_order(int context)
{
    evaluated_context=context;
    evaluated_operation=(int32_t)MEM32(A(0x2125870,0x2BB8D50));
    if (!resolving || commit_consumed || action != SH_TOUCH_TAP || context == 4 ||
        (int32_t)MEM32(A(0x2125870,0x2BB8D50)) == -1) return 0;
    commit_consumed = 1;
    return 1;
}

int stronghold_touch_ability(void)
{
    if (!resolving || commit_consumed || action != SH_TOUCH_AIM_COMMIT) return 0;
    commit_consumed = 1;
    return 1;
}

int stronghold_touch_cancel(void)
{
    if (!resolving || commit_consumed || action != SH_TOUCH_CANCEL) return 0;
    commit_consumed = 1;
    return 1;
}

int stronghold_touch_inspect(void)
{
    return resolving && !commit_consumed && !selection_kind && action == SH_TOUCH_TAP;
}
int stronghold_touch_picker(void) { return resolving == 2; }

uint32_t stronghold_touch_placement(int predicate, uint32_t original)
{
    if (!atomic_load_explicit(&enabled, memory_order_relaxed)) return original;
    /* These are operation predicates inside original placement functions,
     * not writes to mouse state: preview, anchor, and atomic commit. Held
     * placement is deliberately false, so a drag cannot paint buildings. */
    if (!resolving || !aim_known) return 0;
    if (active_callback==CALLBACK_WALL) {
        if (predicate==0) return action==SH_TOUCH_AIM_COMMIT;
        if (predicate==2) return wall_anchor;
        return 0;
    }
    if (active_callback==CALLBACK_DEMOLISH)
        return predicate==1&&action==SH_TOUCH_AIM_COMMIT;
    if (active_callback==CALLBACK_TERRAIN)
        return (predicate==0||predicate==1||predicate==2)&&action==SH_TOUCH_AIM_COMMIT;
    if (predicate == 0) return action == SH_TOUCH_AIM_COMMIT;
    if (predicate == 1) return action == SH_TOUCH_AIM_BEGIN;
    return 0;
}

static void prepare_selection(int same)
{
    uint32_t mode = 5;
    rectangle(aim_x - 2, aim_y - 2, aim_x + 2, aim_y + 2);
    uint32_t id = call(PICK, UNITS, 1, &mode);
    if (!id || id >= MEM32(UNITS) || id > 20000) return;
    uint32_t unit = UNITS + id * 1168u;
    if (MEM16(unit + 1706) != MEM32(PLAYER)) return;
    call(CLEAR, UNITS, 0, NULL);
    call(CLEAR_GROUP, UNITS, 0, NULL);
    if (same) {
        uint32_t args[] = { MEM16(unit + 1706), MEM16(unit + 1698) };
        call(SAME_TYPE, UNITS, 2, args);
        selection_kind = 2;
    } else selection_kind = 1;
}

static void draw_box(void)
{
    rectangle(box_x, box_y, box_end_x, box_end_y);
    int32_t x1 = MEM32(INPUT + 0x70), y1 = MEM32(INPUT + 0x74);
    int32_t x2 = MEM32(INPUT + 0x78), y2 = MEM32(INPUT + 0x7C);
    if (x1 > x2) { int32_t t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int32_t t = y1; y1 = y2; y2 = t; }
    uint32_t drawing = A(0x1A55350,0x248C3A0), flag = drawing + 12, old = MEM32(flag);
    uint32_t args[] = { x1, y1, x2, y2, MEM16(A(0xDF5418,0xDF5598)) };
    MEM32(flag) = 1;
    call(A(0x471750,0x471960), drawing, 5, args);
    args[0]++; args[1]++; args[2]--; args[3]--;
    args[4] = MEM16(A(0xDF53D4,0xDF5554));
    call(A(0x471750,0x471960), drawing, 5, args);
    MEM32(flag) = old;
}

void stronghold_touch_after_world(void)
{
    update_build_highlight();
    if (!atomic_load_explicit(&enabled, memory_order_relaxed) || !in_game()) return;
    if (action == SH_TOUCH_CANCEL) {
        if(classification>=SH_TOUCH_WORLD&&classification<=SH_TOUCH_BUILDING)
            close_world_inspection();
        /* Engine cancellation hook selects its original cancel branch. */
        resolving = 1;
        call(MEM32(BUILD) ? INSPECT : CONTEXT, 0, 0, NULL);
        resolving = 0;
        action = -1; return;
    }
    if (!aim_known) return;
    if (world_blocked()) { action=-1;return; }
    /* World rasterization supplies the object mask; the following original
     * function resolves the precise terrain tile, height and current objects.
     * WinMain normally invokes it later with its desktop cursor coordinates. */
    uint32_t pick_args[]={aim_x,aim_y};
    resolving=2; call(A(0x4E8DD0,0x4E9180),MAP,2,pick_args); resolving=0;
    if (!MEM32(MAP)) {
        if (action==SH_TOUCH_TAP||action==SH_TOUCH_DOUBLE_TAP||action==SH_TOUCH_AIM_COMMIT)
            aim_known=0;
        action=-1;return;
    }
    if(action==SH_TOUCH_TAP||action==SH_TOUCH_DOUBLE_TAP)
        close_world_inspection();
    if (action==SH_TOUCH_TAP && MEM32(MODE)==1 && !MEM32(BUILD) &&
        (callbacks&(1u<<CALLBACK_CONTEXT))) {
        /* Evaluate the original target-specific interaction before choosing
         * ordinary own-unit selection. No operation can commit in this pass.
         * Examples include entering/repairing/interacting with a friendly
         * target: these must not become a plain selection because it is own. */
        int saved_action=action;action=-1;evaluated_context=4;evaluated_operation=-1;
        resolving=1;call(CONTEXT,0,0,NULL);resolving=0;action=saved_action;
        int specific=evaluated_context!=4&&evaluated_operation!=-1&&
                     (evaluated_operation!=0||MEM32(UNITS+16)!=1);
        if (!specific) prepare_selection(0);
    } else if (action == SH_TOUCH_TAP || action == SH_TOUCH_DOUBLE_TAP)
        prepare_selection(action == SH_TOUCH_DOUBLE_TAP);
    if (action == SH_TOUCH_BOX_END && box_active) {
        rectangle(box_x, box_y, box_end_x, box_end_y);
        call(CLEAR, UNITS, 0, NULL); call(CLEAR_GROUP, UNITS, 0, NULL);
        selection_kind = 1; box_active = 0;
    }
    resolving = 1;
    if (MEM32(BUILD)) {
        uint32_t building=MEM32(BUILD);
        if ((building==25||building==26||building==27||building==46)&&(callbacks&(1u<<CALLBACK_WALL))) {
            active_callback=CALLBACK_WALL;
            call(WALL,0,0,NULL);
            wall_anchor=1;
        } else if (building==39&&(callbacks&(1u<<CALLBACK_DEMOLISH))) {
            active_callback=CALLBACK_DEMOLISH;call(DEMOLISH,0,0,NULL);
        } else if (callbacks & (1u << CALLBACK_PLACE)) {
            active_callback=CALLBACK_PLACE;call(PLACE, 0, 0, NULL);
        } else if (callbacks & (1u << CALLBACK_TERRAIN)) {
            active_callback=CALLBACK_TERRAIN;call(TERRAIN, 0, 0, NULL);
        }
    } else if (callbacks & (1u << CALLBACK_CONTEXT)) call(CONTEXT, 0, 0, NULL);
    if (callbacks & (1u << CALLBACK_INSPECT)) call(INSPECT, 0, 0, NULL);
    resolving = 0;
    if (box_active) draw_box();
    if (action == SH_TOUCH_AIM_COMMIT || action == SH_TOUCH_TAP || action == SH_TOUCH_DOUBLE_TAP || action == SH_TOUCH_BOX_END)
        aim_known = wall_anchor = 0;
    action = -1;
    selection_kind = 0;
}

int stronghold_touch_selftest(void)
{
    /* Only called by the isolated shell --selftest, before Android produces
     * events. Exercise event ownership, coalescing, overflow and mode changes. */
    if (queue_count || ui_held || ui_release || ui_drain) return 1;
    int failures=0, old_enabled=atomic_load(&enabled), old_abort=atomic_load(&abort_requested);
    unsigned old_requested=atomic_load(&requested_sequence), old_sequence=sequence;
    uint64_t old_published=atomic_load(&published);
    int old_classification=classification;
#define CHECK(c) do { if (!(c)) { ++failures;void_log("[touch selftest] %s failed",#c); } } while (0)
    queue_head=queue_count=0;
    stronghold_touch_enqueue(SH_TOUCH_QUERY,9,.2f,.3f,0,0,1);
    CHECK(stronghold_touch_status(9)==SH_TOUCH_PENDING);
    CHECK(stronghold_touch_status(8)==SH_TOUCH_BLOCKED);
    stronghold_touch_enqueue(SH_TOUCH_TRANSFORM,9,.3f,.4f,.1f,.2f,1.1f);
    stronghold_touch_enqueue(SH_TOUCH_TRANSFORM,9,.4f,.5f,.2f,.3f,.9f);
    CHECK(queue_count==2);
    CHECK(fabsf(queue[1].dx-.3f)<.00001f&&fabsf(queue[1].dy-.5f)<.00001f&&fabsf(queue[1].scale-.99f)<.00001f);
    stronghold_touch_enqueue(SH_TOUCH_TAP,9,.4f,.5f,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_TAP,9,.4f,.5f,0,0,1);
    CHECK(queue_count==4); /* command edges cannot coalesce */
    queue_head=queue_count=0;
    stronghold_touch_enqueue(SH_TOUCH_UI_SCROLL,9,.2f,.3f,.1f,.2f,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_SCROLL,9,.4f,.5f,.2f,.3f,1);
    CHECK(queue_count==1&&queue[0].x==.2f&&queue[0].y==.3f);
    CHECK(fabsf(queue[0].dx-.3f)<.00001f&&fabsf(queue[0].dy-.5f)<.00001f);
    stronghold_touch_enqueue(SH_TOUCH_UI_CLICK,9,.4f,.5f,0,0,1);
    stronghold_touch_enqueue(SH_TOUCH_UI_CLICK,9,.4f,.5f,0,0,1);
    CHECK(queue_count==3);
    queue_head=queue_count=0;
    stronghold_touch_enqueue(SH_TOUCH_WHEEL,0,.2f,.3f,120,1,1.15f);
    stronghold_touch_enqueue(SH_TOUCH_WHEEL,0,.4f,.5f,120,0,1.15f);
    CHECK(queue_count==1&&queue[0].x==.4f&&queue[0].dx==240&&queue[0].dy==1&&fabsf(queue[0].scale-1.3225f)<.0001f);
    queue_head=queue_count=0;
    for (int i=0;i<=QUEUE_SIZE;i++) stronghold_touch_enqueue(SH_TOUCH_TAP,9,.4f,.5f,0,0,1);
    CHECK(queue_count==0&&atomic_load(&abort_requested)==1);
    CHECK(stronghold_touch_status(9)==SH_TOUCH_BLOCKED);
    atomic_store(&abort_requested,0);
    stronghold_touch_enqueue(SH_TOUCH_MODE,0,0,0,0,0,0);
    stronghold_touch_frame();
    CHECK(!atomic_load(&enabled)&&stronghold_touch_legacy(123)==123);
    stronghold_touch_enqueue(SH_TOUCH_UI_SCALE,0,0,0,0,0,1);
    CHECK(!atomic_load(&enabled));
    queue_head=queue_count=0;
    stronghold_touch_enqueue(SH_TOUCH_ABORT,9,0,0,0,0,1);
    stronghold_touch_frame();
    CHECK(!atomic_load(&enabled));
    stronghold_touch_enqueue(SH_TOUCH_MODE,0,0,0,0,0,1);
    stronghold_touch_frame();
    CHECK(atomic_load(&enabled)&&stronghold_touch_legacy(123)==0);
    queue_head=queue_count=0;
    atomic_store(&enabled,old_enabled);atomic_store(&abort_requested,old_abort);
    atomic_store(&requested_sequence,old_requested);atomic_store(&published,old_published);
    sequence=old_sequence;classification=old_classification;
    callbacks=0;action=-1;aim_known=box_active=selection_kind=0;
    void_log("[touch selftest] queue, stale sequence, gesture overflow, transform/wheel coalescing, abort and mode: %s (0=PASS)",failures?"FAIL":"PASS");
    return failures;
#undef CHECK
}
