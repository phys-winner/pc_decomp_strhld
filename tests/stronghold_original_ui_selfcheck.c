/* Geometry and inverse-hit regression test, using the production code.
 * Build with the Android NDK:
 * clang --target=aarch64-linux-android30 -O2 -std=gnu11 -ffunction-sections
 *   -fdata-sections -Iandroid/runtime -Iandroid/host
 *   tests/stronghold_original_ui_selfcheck.c -Wl,--gc-sections -lm -o check
 * Run the resulting executable through adb shell. */
#define STRONGHOLD_ORIGINAL
#include "../android/host/stronghold_original.c"
#include <assert.h>
#include <sys/mman.h>


void void_log(const char *format, ...) { (void)format; }
static int screen_readable;
int void_vm_readable(uint32_t address, uint32_t size)
{ return screen_readable && address >= 0xB18000u && (uint64_t)address + size <= 0xB19000u; }
uint32_t void_vm_alloc(uint32_t address, uint32_t size, uint32_t type, uint32_t protect)
{ (void)address; (void)size; (void)type; (void)protect; abort(); }
static int redirects;
uint32_t void_ddraw_redirect_pixels(uint32_t surface, uint32_t pixels)
{ (void)surface; (void)pixels; ++redirects; return 1; }

static void before_image_commit(void)
{
    void *page = GPTR(0xB18000u);
    assert(mmap(page, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == page);
    frame_ready = 0; surface_width = 2340; surface_height = 1080;
    float x = 31, y = 41, scale = 51;
    for (int group = -1; group < SH_UI_LAYERS; ++group) {
        assert(!stronghold_display_layer_view(group, 652, 300, &x, &y, &scale));
        assert(x == 31 && y == 41 && scale == 51);
    }
    /* The menu's unlayered mapping is still available after the image maps. */
    assert(!mprotect(page, 4096, PROT_READ | PROT_WRITE)); screen_readable = 1;
    MEM32(SCREEN_WIDTH) = 1304; MEM32(SCREEN_HEIGHT) = 600;
    assert(stronghold_display_layer_view(-1, 652, 300, &x, &y, &scale));
    assert(fabsf(x - 1170) < .001f && fabsf(y - 540) < .001f);
    assert(fabsf(scale - 2340.f / 1304.f) < .001f);
    assert(!stronghold_display_layer_view(0, 652, 300, &x, &y, &scale));
    /* Published geometry is host state and does not need a guest fallback. */
    frame_ready = 1; published_width = 1304; published_height = 600;
    screen_readable = 0; assert(!mprotect(page, 4096, PROT_NONE));
    assert(stronghold_display_layer_view(-1, 652, 300, &x, &y, &scale));
    assert(fabsf(x - 1170) < .001f && fabsf(y - 540) < .001f);
    frame_ready = 0; published_width = published_height = 0; surface_width = surface_height = 0;
    assert(!munmap(page, 4096));
    puts("Stronghold HUD point: before-image PROT_NONE guard, mapped menu fallback and published host geometry PASS");
}

static void speed_notice(void)
{
    const uint32_t pages[] = {0xA3B000,0xB18000,0x16E9000};
    for (int i=0;i<3;i++) assert(mmap(GPTR(pages[i]),4096,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0) == GPTR(pages[i]));
    /* 552830 initializes id24 at770,9, flags0; 490160 adds W-800 for right, nothing for top. */
    MEM32(SPEED_NOTICE)=770;MEM32(SPEED_NOTICE+4)=9;MEM32(SPEED_NOTICE+8)=24;
    anchor_speed_notice();assert(MEM32(SPEED_NOTICE+0x1C)==9);
    MEM32(SCREEN_WIDTH)=frame_width=1560;MEM32(SCREEN_HEIGHT)=frame_height=720;
    int x=(int)MEM32(SPEED_NOTICE)+frame_width-800,y=(int)MEM32(SPEED_NOTICE+4);
    assert(x==frame_width-30 && y==9);
    frame_open=1;layers[3].used=1;layers[3].guest=0x10000000;
    redirects=0;
    stronghold_original_item(SPEED_NOTICE,1);assert(current==3 && stronghold_display_capture_active && redirects==1);
    /* The next overlay of the same layer keeps the redirect; the loop's end closes it. */
    stronghold_original_item(SPEED_NOTICE,0);assert(current==3);
    stronghold_original_item(SPEED_NOTICE,1);assert(current==3 && redirects==1);
    stronghold_original_items_end();assert(current==-1 && !stronghold_display_capture_active && redirects==2);
    /* The empty callback (nullsub 45C870) opens no layer. */
    MEM32(SPEED_NOTICE+0x18)=OVERLAY_NONE;stronghold_original_item(SPEED_NOTICE,1);assert(current==-1 && redirects==2);
    MEM32(SPEED_NOTICE+0x18)=0x42D170;
    const int flags[] = {5,6,9,1,8,0,2}, groups[] = {1,4,3,5,6,2,0};
    for (int k=0;k<7;k++) {
        layers[groups[k]].used=1;layers[groups[k]].guest=0x10000000u+groups[k]*0x1000000u;
        MEM32(SPEED_NOTICE+0x1C)=flags[k];stronghold_original_item(SPEED_NOTICE,1);
        assert(current==groups[k]);stronghold_original_items_end();
    }
    for(int id=26;id<=27;id++){
        MEM32(SPEED_NOTICE+8)=id;MEM32(SPEED_NOTICE+0x1C)=1;
        stronghold_original_item(SPEED_NOTICE,1);assert(current==1);stronghold_original_items_end();
    }
    frame_open=0;
    MEM32(SPEED_NOTICE+8)=23;MEM32(SPEED_NOTICE+0x1C)=0;
    anchor_speed_notice();assert(MEM32(SPEED_NOTICE+0x1C)==0);
    for(int i=0;i<3;i++)assert(!munmap(GPTR(pages[i]),4096));
    puts("Original speed notice: guarded id24 anchor, original width/height arithmetic, real capture route to right layer PASS");
}

static void reset(int w, int h)
{
    for (int i = 0; i < SH_UI_LAYERS; ++i) {
        free(layers[i].coverage);
        layers[i] = (ui_layer){0};
        layers[i].coverage = calloc((size_t)w * h, 1);
        assert(layers[i].coverage);
        published_used[i] = 1;
    }
    boxes = 0; top_fit = (stronghold_rect){0}; frame_width = published_width = w; frame_height = published_height = h;
    frame_ready = 1; ui_scale = 2;
    layers[0].shown = (stronghold_rect){0, h - HUD_HEIGHT, w, h};
}
static void near(double a, double b) { assert(fabs(a - b) < .001); }
static void inside(float anchor, int low, int high, int limit)
{
    assert(anchor + (low - anchor) * published_scale >= -.001);
    assert(anchor + (high - anchor) * published_scale <= limit + .001);
}
static void hit(int group, int x, int y, int bottom)
{
    stronghold_part parts[SH_UI_PARTS], chosen;
    int n = layer_parts(parts, frame_width, frame_height), p = -1;
    for (int i = 0; i < n; ++i) if (parts[i].layer == group && y >= parts[i].top && y < parts[i].bottom) p = i;
    assert(p >= 0);
    layers[group].coverage[(size_t)y * frame_width + x] = 255;
    double fx = parts[p].anchor_x + (x + .5 - parts[p].anchor_x) * published_scale;
    double fy = parts[p].anchor_y + (y + .5 - parts[p].anchor_y) * published_scale;
    assert(fx >= 0 && fx < frame_width && fy >= 0 && fy < frame_height);
    assert(pick_part(fx, fy, &chosen) == p);
    double sx, sy; part_source(&chosen, fx, fy, published_scale, &sx, &sy);
    near(sx, x + .5); near(sy, y + .5);
    if (group == 3) { near(chosen.anchor_x, frame_width); near(chosen.anchor_y, 0); }
    if (group == 4) { near(chosen.anchor_x, 0); near(chosen.anchor_y, frame_height); }
    (void)bottom;
    surface_width = 2340; surface_height = 1080;
    double fit = fmin((double)surface_width / frame_width, (double)surface_height / frame_height);
    int gx, gy;
    float nx = (float)((fx * fit + (surface_width - frame_width * fit) * .5) / surface_width);
    float ny = (float)((fy * fit + (surface_height - frame_height * fit) * .5) / surface_height);
    assert(stronghold_display_map_point(nx, ny, &gx, &gy) && gx == x && gy == y);
    layers[group].coverage[(size_t)y * frame_width + x] = 0;
}
static void registered_overlays(void)
{
    const int sizes[][2]={{800,600},{1064,600},{1280,720},{1560,720},{1664,768},{2336,1080}};
    const float scales[]={1,1.5f,2};
    for (unsigned z=0;z<sizeof sizes/sizeof sizes[0];z++) for(unsigned k=0;k<3;k++) {
        int w=sizes[z][0],h=sizes[z][1];reset(w,h);ui_scale=scales[k];
        /* Actual42D170 emits13 nonzero resource rows at y10+30*i;
         * constructor12 uses flags5, so the last rows cross h/2 at720.
         * Actual490280 wraps warning text to520px;490790 draws480x72;
         * 490E60's right-centre network panel is120x192. */
        top_fit=layers[1].shown=(stronghold_rect){4,4,200,390};
        layers[4].shown=(stronghold_rect){10,h-245,530,h-165};
        layers[5].shown=(stronghold_rect){w/2-240,11,w/2+240,83};
        layers[6].shown=(stronghold_rect){w-140,h/2-260,w-20,h/2-68};
        layers[2].shown=(stronghold_rect){w/2-150,h/2-80,w/2+150,h/2+80};
        publish_geometry(1);
        float resources_bottom=published_edge_anchor[1][1]+(layers[1].shown.bottom-published_edge_anchor[1][1])*published_scale;
        float panel_top=h+(layers[0].shown.top-h)*published_scale;
        assert(resources_bottom<=panel_top+.001f);
        stronghold_part parts[SH_UI_PARTS];int n=layer_parts(parts,w,h);assert(n==7);
        for(int p=0;p<n;p++) {
            int g=parts[p].layer;assert(g==SH_UI_ORDER[p]&&parts[p].top==0&&parts[p].bottom==h);
            if(!g||!layers[g].shown.right)continue;
            stronghold_rect b=layers[g].shown;
            inside(parts[p].anchor_x,b.left,b.right,w);inside(parts[p].anchor_y,b.top,b.bottom,h);
        }
        /* The old half-height heuristic changed these rows from299/301 to
         * 448.5/151.5 at150%. One complete part is monotonic across that cut. */
        float vx0,vy0,vx1,vy1,s0,s1;surface_width=w;surface_height=h;
        assert(stronghold_display_layer_view(1,34,299,&vx0,&vy0,&s0));
        assert(stronghold_display_layer_view(1,34,301,&vx1,&vy1,&s1));
        near(vy1-vy0,2*published_scale);near(s0,s1);
        hit(1,34,340,0);hit(1,34,370,0);hit(4,20,h-220,1);
        hit(5,w/2,30,0);hit(6,w-60,h/2-150,0);
        float top_x=published_edge_anchor[5][0],top_y=published_edge_anchor[5][1];
        /* An independent displaced modal may move itself, never the top notice. */
        layers[2].shown=(stronghold_rect){w-310,h/2-80,w-10,h/2+80};publish_geometry(1);
        near(published_edge_anchor[5][0],top_x);near(published_edge_anchor[5][1],top_y);
    }
    puts("HD registered overlays100..200%:13 original resource rows beyond300,520px warning, centre-top/modal isolation, right-centre fit, monotonic rows and inverse hits PASS");
}
/* Translucent stores over the transparent fill compose to the same picture as
 * the original drawing over the world: shading keeps factor/32 of the pixel
 * below, a masked sprite pixel at level a keeps (32 - a)/32 of it. */
static unsigned channel(uint16_t c, int i) { return i == 0 ? c >> 11 : i == 1 ? (c >> 5) & 63 : c & 31; }
static void close_to(uint16_t got, uint16_t want)
{ for (int i = 0; i < 3; ++i) assert(abs((int)channel(got, i) - (int)channel(want, i)) <= 1); }
static uint16_t scaled(uint16_t c, int f)
{ return (uint16_t)((channel(c, 0) * f / 32) << 11 | (channel(c, 1) * f / 32) << 5 | channel(c, 2) * f / 32); }
static uint16_t mixed(uint16_t a, uint16_t b, int f)
{
    unsigned r = (channel(a, 0) * f + channel(b, 0) * (32 - f)) / 32, g = (channel(a, 1) * f + channel(b, 1) * (32 - f)) / 32;
    return (uint16_t)(r << 11 | g << 5 | (channel(a, 2) * f + channel(b, 2) * (32 - f)) / 32);
}
static void translucent(void)
{
    frame_width = 64; frame_height = 16; origin_x = 5; origin_y = 3; frame_open = 1; key = 0xF81F;
    ui_layer *l = &layers[2];
    l->guest = 0x10000000; l->capture = calloc((size_t)frame_width * frame_height, 1); assert(l->capture);
    l->left = frame_width; l->top = frame_height; l->right = l->bottom = 0;
    current = 2; stronghold_display_capture_active = 1;
    const uint16_t worlds[] = {0x0000, 0xFFFF, 0x07E0, 0x39E7, 0xA554, 0xF81F, 0x1234};
    for (int f = 1; f < 32; ++f) for (size_t k = 0; k < sizeof worlds / sizeof worlds[0]; ++k) {
        int x = f, y = (int)k;
        uint32_t at = l->guest + RASTER_PITCH * (uint32_t)(origin_y + y) + 2u * (uint32_t)(origin_x + x);
        l->capture[y * frame_width + x] = 0;
        /* 442520 / 459DC0: the fill shaded to f/32, background weight f*255/32. */
        stronghold_display_blended(at, 2, (unsigned)f * 255u / 32u);
        unsigned a = l->capture[y * frame_width + x];
        close_to(stronghold_composite_pixel(scaled(key, f), worlds[k], a, key), scaled(worlds[k], f));
        /* 442670: a sprite pixel at mask level f over the fill. */
        l->capture[y * frame_width + x] = 0;
        stronghold_display_blended(at, 2, (32u - (unsigned)f) * 255u / 32u);
        a = l->capture[y * frame_width + x];
        close_to(stronghold_composite_pixel(mixed(0x7BEF, key, f), worlds[k], a, key), mixed(0x7BEF, worlds[k], f));
    }
    assert(l->left == 1 && l->right == 32 && l->top == 0 && l->bottom == 7);
    /* A store outside the open layer's raster is not coverage. */
    stronghold_display_blended(l->guest - 2, 2, 0); stronghold_display_blended(0x00B18300u, 4, 0);
    assert(l->left == 1 && l->top == 0);
    free(l->capture); *l = (ui_layer){0};
    current = -1; stronghold_display_capture_active = 0; frame_open = 0; origin_x = origin_y = 0;
    puts("Translucent coverage: shaded fill and masked sprite levels 1..31 compose as over the world PASS");
}
int main(void)
{
    assert(stronghold_notice_margin(1600,720,2400,1080,0,1)==16);
    assert(stronghold_notice_margin(1600,720,2400,1080,.05f,1)==96);
    assert(stronghold_notice_margin(1280,720,2400,1080,.05f,1)==16);
    assert(stronghold_notice_margin(1600,720,2400,1080,.05f,.75f)==123);
    before_image_commit();
    speed_notice();
    translucent();
    const int sizes[][2] = {{800,600},{1064,600},{1280,720},{1560,720},{1664,768},{1944,900},{2336,1080},{2560,1600}};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        int w = sizes[i][0], h = sizes[i][1]; reset(w,h); publish_geometry(1);
        near(published_scale, fmax(1, fmin(2, w / 960.)));
        inside(w*.5f, (w-800)/2, (w+800)/2, w);
        /* The right book leaf is part of edge*r.tgx, outside the nominal 800px box. */
        if (w >= 960) {
            inside(w*.5f, w/2-400, w/2+480, w);
            hit(0,w/2+479,h-30,0);
        }
        stronghold_part parts[SH_UI_PARTS]; assert(layer_parts(parts,w,h) == 7);
        assert(parts[0].top == 0 && parts[6].layer == 2);
        hit(1,30,30,0); hit(4,30,h-30,1); hit(3,w-30,30,0); hit(5,w/2,30,0); hit(6,w-30,h/2,0);
        /* A displaced dialog must not move either right-side anchor. */
        boxes = 1; box[0][0]=w-410;box[0][1]=150;box[0][2]=w-10;box[0][3]=450;box[0][4]=0;
        layers[2].shown=(stronghold_rect){w-410,150,w-10,450}; publish_geometry(1);
        inside(published_anchor[0],w-410,w-10,w);inside(published_anchor[1],150,450,h);
        hit(3,w-30,30,0);hit(4,30,h-30,1);hit(2,w-200,300,0);
        /* A tall dialog is capped, even when it needs nearly the whole frame. */
        box[0][0]=(w-400)/2;box[0][1]=5;box[0][2]=(w+400)/2;box[0][3]=h-5;
        layers[2].shown=(stronghold_rect){box[0][0],5,box[0][2],h-5}; publish_geometry(1);
        assert(published_scale <= (float)h/(h-10)+.00001f);
        inside(published_anchor[0],box[0][0],box[0][2],w);inside(published_anchor[1],5,h-5,h);
        /* A tall box attached to the panel keeps its rows above the old 128px split. */
        box[0][4]=1;layers[2].shown=(stronghold_rect){0};layers[0].shown.top=10;publish_geometry(1);
        inside((float)h,10,h,h);hit(0,w/2,15,0);
        stronghold_display_set_ui_scale(1);publish_geometry(1);near(published_scale,1);
        stronghold_display_set_ui_scale(NAN);near(ui_scale,1);
    }
    puts("Stronghold UI: panel/dialog fit, seven original anchor classes, dialog independence, top overhang, inverse hits/letterbox PASS");
    registered_overlays();
    return 0;
}
