/* HD production input path: ordinary preview is unpressed, wall preview held,
 * cancellation precedes release. Separate process, without a game activity. */
#define STRONGHOLD_ORIGINAL
#include "../android/host/stronghold_original.c"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

static _Atomic int cursor_x, cursor_y, events;
static struct { int button, down, x, y; unsigned frame; } event[128];
int void_vm_readable(uint32_t address, uint32_t size)
{ return address >= 0x400000u && (uint64_t)address + size <= 0x2000000u; }
void void_input_screen_size(int *w, int *h) { *w = 1200; *h = 600; }
void void_input_mouse_warp(int x, int y) { cursor_x = x; cursor_y = y; }
void void_input_mouse_button(int b, int down)
{
    assert(events < (int)(sizeof event / sizeof *event));
    int index = events;
    event[index] = (typeof(*event)){b, down, cursor_x, cursor_y, frames};
    events = index + 1;
}
void void_input_mouse_wheel(int delta) { (void)delta; }
void void_input_key(uint32_t key, int down) { (void)key; (void)down; }
uint64_t void_monotonic_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec; }
int stronghold_original_build_panel(int x, int y) { (void)x; (void)y; return 0; }
void stronghold_original_build_swipe(int direction) { (void)direction; }

static void send(int action, unsigned sequence, float x, float y)
{ stronghold_touch_enqueue(action, sequence, x, y, 0, 0, 1); }
static void query(unsigned sequence, uint32_t build)
{
    MEM32(STATE) = 14; MEM32(DIALOG) = UINT32_MAX; MEM32(0x1503B24u) = build;
    send(SH_TOUCH_QUERY, sequence, .25f, .25f);
}
static void drain(void)
{
    frames += 2;
    pthread_mutex_lock(&lock);
    if (cancelling_aim) { MEM32(0x1503B24u) = 0; original_cancel_frame(); } /* Original42D630 acknowledgement. */
    pthread_cond_signal(&button_cond); pthread_mutex_unlock(&lock);
    for (int i = 0; i < 100; ++i) {
        usleep(10000);
        pthread_mutex_lock(&lock); int done = button < 0; pthread_mutex_unlock(&lock);
        if (done) return;
    }
    assert(!"sampled mouse release timed out");
}
static void loupe_and_highlight(void)
{
    float x = -1, y = -1;
    assert(!stronghold_display_loupe(&x, &y));
    stronghold_display_magnifier(1, -.2f, 1.2f);
    assert(stronghold_display_loupe(&x, &y) && x == 0 && y == 1);
    stronghold_display_magnifier(1, NAN, .2f); assert(!stronghold_display_loupe(&x, &y));
    stronghold_display_magnifier(1, .4f, .6f);
    assert(stronghold_display_loupe(&x, &y) && x == .4f && y == .6f);
    stronghold_display_magnifier(0, .2f, .3f); assert(!stronghold_display_loupe(&x, &y));
    stronghold_display_highlight(500, 510, 45, 30);
    frame_ready = 1; frame_taken = 0; published_width = 1200; published_height = 600; published_scale = 1.5f;
    published_used[0] = 1;
    stronghold_frame f;
    assert(stronghold_display_frame(&f, 1200, 600, 16) == 1);
    assert(f.highlight.left == 500 && f.highlight.top == 510 && f.highlight.right == 545 && f.highlight.bottom == 540);
    assert(f.ui_scale == 1.5f && f.part[0].anchor_x == 600 && f.part[0].anchor_y == 600);
    stronghold_display_highlight(0, 0, -1, 30); frame_taken = 0;
    assert(stronghold_display_frame(&f, 1200, 600, 16) == 1 && !f.highlight.right);
    stronghold_display_highlight(500, 510, 45, 30); published_used[0] = 0; frame_taken = 0;
    assert(stronghold_display_frame(&f, 1200, 600, 16) == 1 && !f.highlight.right);
    frame_ready = 0;
}
static void radar_geometry(void)
{
    uint32_t owner=0x600000,radar=0x5A1240,book=0x5A16F0;
    MEM32(book+76)=owner;MEM32(book+20)=0x43ADA0;MEM32(book+24)=71;
    MEM32(book+4)=815;MEM32(book+8)=503;MEM32(book+12)=110;MEM32(book+16)=97;
    frame_width=published_width=1200;frame_height=published_height=600;
    frame_ready=published_used[0]=1;
    layers[0].coverage=calloc(1200*600,1);assert(layers[0].coverage);
    const float positions[]={.01f,.1f,.5f,.95f,.99f},scales[]={1,1.2f,2};
    unsigned sequence=800;
    for(int letterbox=0;letterbox<2;letterbox++)for(int s=0;s<3;s++) {
        surface_width=letterbox?2600:2400;published_scale=scales[s];
        for(int y=0;y<5;y++)for(int x=0;x<5;x++) {
            float rx=700+126*positions[x],ry=460+126*positions[y],nx,ny,scale;
            assert(stronghold_display_layer_view(0,rx,ry,&nx,&ny,&scale));
            nx/=surface_width;ny/=surface_height;
            send(SH_TOUCH_QUERY,++sequence,nx,ny);
            assert(current_kind==SH_TOUCH_RADAR&&touch_layer);
            assert(abs(start_x-(int)rx)<=1&&abs(start_y-(int)ry)<=1);
            pan_x=pan_y=0;
            stronghold_touch_enqueue(SH_TOUCH_PAN,sequence,nx,ny,.05f,.02f,1);
            assert(fabsf(pan_x-.05f*surface_width)<.001f&&fabsf(pan_y-24)<.001f);
        }
    }
    MEM16(radar+54)=1;assert(original_hud_kind(825,550)==SH_TOUCH_BOOK);MEM16(radar+54)=0;
    assert(original_hud_kind(825,550)==SH_TOUCH_RADAR);
    assert(original_hud_kind(827,550)==SH_TOUCH_BOOK);
    MEM32(DIALOG)=5;assert(original_hud_kind(825,550)==SH_TOUCH_MENU);MEM32(DIALOG)=UINT32_MAX;
    MEM32(owner+24)=1;assert(original_hud_kind(825,550)==SH_TOUCH_UI);MEM32(owner+24)=0;
    free(layers[0].coverage);layers[0].coverage=NULL;frame_ready=published_used[0]=touch_layer=0;
    MEM32(book+76)=0;surface_width=2400;
    query(++sequence,0);pan_x=pan_y=0;
    stronghold_touch_enqueue(SH_TOUCH_PAN,sequence,.25f,.25f,.05f,.02f,1);
    assert(fabsf(pan_x-60)<.001f&&fabsf(pan_y-12)<.001f);pan_x=pan_y=0;
    puts("HD radar: 150 transparent-pixel queries, all edges, book overlap, UI100/120/200, letterbox, 2x radar and unchanged world gain PASS");
}
int main(void)
{
    assert(mmap(GPTR(0x400000), 0x1C00000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    surface_width = 2400; surface_height = 1200;
    loupe_and_highlight();
    MEM32(STATE)=14;MEM32(DIALOG)=UINT32_MAX;MEM32(STATE+4)=10;
    uint32_t owner=0x600000,radar=0x5A1240;
    MEM32(0xABB974)=owner;MEM32(radar+76)=owner;MEM32(radar+20)=0x42D590;
    MEM32(radar+4)=700;MEM32(radar+8)=460;MEM32(radar+12)=126;MEM32(radar+16)=126;
    current_sequence=700;current_kind=SH_TOUCH_RADAR;start_x=720;start_y=480;
    pan_x=pan_y=0;
    stronghold_touch_enqueue(SH_TOUCH_PAN,700,.6f,.8f,.05f,.02f,1);
    assert(fabsf(pan_x-120)<.001f&&fabsf(pan_y-24)<.001f);
    MEM32(DIALOG)=5;
    stronghold_touch_enqueue(SH_TOUCH_PAN,700,.6f,.8f,.05f,.02f,1);
    assert(fabsf(pan_x-120)<.001f&&fabsf(pan_y-24)<.001f);
    MEM32(DIALOG)=UINT32_MAX;MEM16(radar+54)=1;
    stronghold_touch_enqueue(SH_TOUCH_PAN,700,.6f,.8f,.05f,.02f,1);
    assert(fabsf(pan_x-120)<.001f&&fabsf(pan_y-24)<.001f);
    MEM16(radar+54)=0;pan_x=pan_y=0;
    radar_geometry();
    puts("HD radar: relative pan, dialog and hidden-control guards PASS");
    query(1, 0); assert(stronghold_touch_status(1) == SH_TOUCH_WORLD);
    query(2, 7); assert(stronghold_touch_status(2) == SH_TOUCH_BUILDING);
    send(SH_TOUCH_QUERY, 3, .25f, .9f); assert(stronghold_touch_status(3) == SH_TOUCH_UI);
    MEM32(DIALOG) = 3; send(SH_TOUCH_QUERY, 4, .25f, .25f);
    assert(stronghold_touch_status(4) == SH_TOUCH_UI);

    query(5, 7); send(SH_TOUCH_AIM_BEGIN, 5, .25f, .25f);
    send(SH_TOUCH_AIM_UPDATE, 5, .5f, .4f);
    assert(!events && cursor_x == 600 && cursor_y == 240 && button < 0);
    send(SH_TOUCH_AIM_COMMIT, 4, .5f, .4f); assert(!events && aim_build == 7);
    send(SH_TOUCH_AIM_COMMIT, 5, .5f, .4f); drain();
    assert(events == 2 && event[0].button == 0 && event[0].down && !event[1].down);
    assert(event[0].x == 600 && event[0].y == 240 && event[1].x == 600 && event[1].y == 240);
    assert(event[1].frame - event[0].frame >= 2 && !aim_build);

    const uint32_t areas[] = {25, 46, 27, 26};
    for (unsigned i = 0; i < sizeof areas / sizeof *areas; ++i) {
        int before = events; unsigned sequence = 10 + i;
        query(sequence, areas[i]); send(SH_TOUCH_AIM_BEGIN, sequence, .25f, .25f);
        send(SH_TOUCH_AIM_UPDATE, sequence, .5f, .4f);
        assert(events == before + 1 && button == 0 && !releasing);
        frames += 2;
        pthread_mutex_lock(&lock); pthread_cond_signal(&button_cond); pthread_mutex_unlock(&lock);
        usleep(20000);
        assert(events == before + 1 && cursor_x == 600 && cursor_y == 240);
        send(SH_TOUCH_AIM_COMMIT, sequence, .5f, .4f); drain();
        assert(events == before + 2 && event[before].down && !event[before + 1].down);
        assert(event[before].x == 300 && event[before].y == 150);
        assert(event[before + 1].x == 600 && event[before + 1].y == 240);
    }
    /* Demolition deletes on original down: keep its aim unpressed until commit. */
    int before = events;
    query(19, 39); send(SH_TOUCH_AIM_BEGIN, 19, .25f, .25f);
    send(SH_TOUCH_AIM_UPDATE, 19, .5f, .4f); assert(events == before && button < 0);
    send(SH_TOUCH_AIM_COMMIT, 19, .5f, .4f); drain();
    assert(events == before + 2 && event[before].x == 600 && event[before + 1].x == 600);
    before = events;
    query(20, 27); send(SH_TOUCH_AIM_BEGIN, 20, .25f, .25f);
    send(SH_TOUCH_AIM_UPDATE, 20, .5f, .4f); send(SH_TOUCH_CANCEL, 20, .5f, .4f);
    assert(events == before + 2 && event[before].button == 0 && event[before].down &&
        event[before + 1].button == 1 && event[before + 1].down && button == 0 && cancelling_aim);
    usleep(60000); assert(events == before + 2); /* No frame sample: do not release left. */
    send(SH_TOUCH_QUERY, 200, .5f, .4f); assert(stronghold_touch_status(200) == SH_TOUCH_BLOCKED);
    drain();
    assert(events == before + 4 && event[before + 2].button == 0 && !event[before + 2].down &&
        event[before + 3].button == 1 && !event[before + 3].down);
    assert(event[before + 2].frame - event[before + 1].frame >= 2);
    /* A swallowed RMB must not release left. Once the dialog closes, repeat
     * an actual sampled right-up/right-down, then wait for original BUILD=0. */
    before = events; query(201, 25); send(SH_TOUCH_AIM_BEGIN, 201, .25f, .25f);
    send(SH_TOUCH_ABORT, 201, .5f, .4f);
    MEM32(DIALOG) = 3; frames += 4;
    pthread_mutex_lock(&lock); original_cancel_frame(); pthread_mutex_unlock(&lock);
    usleep(60000); assert(events == before + 2 && button == 0);
    MEM32(DIALOG) = UINT32_MAX;
    pthread_mutex_lock(&lock); original_cancel_frame(); pthread_mutex_unlock(&lock);
    assert(events == before + 3 && event[before + 2].button == 1 && !event[before + 2].down);
    frames += 2;
    pthread_mutex_lock(&lock); original_cancel_frame(); pthread_mutex_unlock(&lock);
    assert(events == before + 4 && event[before + 3].button == 1 && event[before + 3].down && button == 0);
    drain(); assert(events == before + 6 && event[before + 4].button == 0 && !event[before + 4].down);
    before = events;
    query(21, 7); send(SH_TOUCH_AIM_BEGIN, 21, .25f, .25f);
    MEM32(DIALOG) = 3; send(SH_TOUCH_AIM_COMMIT, 21, .5f, .4f); assert(events == before);
    query(22, 7); send(SH_TOUCH_AIM_BEGIN, 22, .25f, .25f);
    MEM32(0x1503B24u) = 8; send(SH_TOUCH_AIM_COMMIT, 22, .5f, .4f); assert(events == before);
    query(23, 27); send(SH_TOUCH_AIM_BEGIN, 23, .25f, .25f);
    stronghold_display_magnifier(1, .5f, .4f); send(SH_TOUCH_ABORT, 23, .5f, .4f);
    float x, y; assert(events == before + 2 && !aim_build && cancelling_aim && !stronghold_display_loupe(&x, &y));
    drain(); assert(events == before + 4); before = events;
    query(24, 26); send(SH_TOUCH_AIM_BEGIN, 24, .25f, .25f);
    stronghold_display_magnifier(1, .5f, .4f); stronghold_touch_abort();
    assert(events == before + 2 && !aim_build && cancelling_aim && !stronghold_display_loupe(&x, &y));
    drain(); assert(events == before + 4);
    assert(stronghold_touch_status(24) == SH_TOUCH_BLOCKED);
    before = events;
    pthread_mutex_lock(&lock); int saved_worker = worker; worker = -1; pthread_mutex_unlock(&lock);
    query(25, 27); send(SH_TOUCH_AIM_BEGIN, 25, .25f, .25f); stronghold_touch_abort();
    pthread_mutex_lock(&lock);
    MEM32(0x1503B24u) = 0; frames += 2; release_ns -= 50000000u; original_cancel_frame();
    assert(button < 0 && !cancelling_aim); worker = saved_worker;
    pthread_mutex_unlock(&lock);
    assert(events == before + 4); /* Guest frame also cleans up if pthread_create failed. */
    before = events;
    query(26, 25); send(SH_TOUCH_AIM_BEGIN, 26, .25f, .25f); stronghold_touch_abort();
    MEM32(STATE) = 41; /* Stale BUILD may survive an independently changed scene. */
    pthread_mutex_lock(&lock); original_cancel_frame(); pthread_mutex_unlock(&lock);
    assert(button < 0 && !cancelling_aim && MEM32(0x1503B24u) == 25 && events == before + 4);
    send(SH_TOUCH_QUERY, 27, .5f, .4f); assert(stronghold_touch_status(27) == SH_TOUCH_UI);
    puts("HD AIM: armed world/UI guards, unpressed ordinary/demolition aim, live held wall/stair preview, original commit edges, RMB-before-LUP cancel/abort with two sampled frames and blocked reentry; stale/modal/type guards, finite loupe and scaled HUD highlight PASS");
    return 0;
}
