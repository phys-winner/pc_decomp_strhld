/* Stronghold HD's real seven-layer capture and crisp-text pipeline.
 * NDK: -O2 -std=gnu11 -ffunction-sections -fdata-sections -Iandroid/runtime
 * -Iandroid/host tests/stronghold_original_text_selfcheck.c -Wl,--gc-sections -lm -lEGL -lGLESv3 */
#define STRONGHOLD_ORIGINAL
#include "../android/host/stronghold_original.c"
#define void_android_text_raster unused_android_text_raster
#include "../android/host/gdi_android.c"
#undef void_android_text_raster
#include "../android/host/text_overlay.c"
#include <EGL/egl.h>
#include <assert.h>
#include <sys/mman.h>

#define W 1280
#define H 720
#define WORLD 0x20000000u
#define GROUPS 0x22000000u
#define HUD 0x2a000000u
#define HUD_PITCH (W * 2u + 32u)
static uint32_t next_group = GROUPS;
int void_verbose;
void void_log(const char *format, ...) { (void)format; }
uint64_t void_monotonic_ns(void) { return 0; }
void stronghold_perf_frame_begin(void) {}
uint32_t void_vm_alloc(uint32_t address, uint32_t size, uint32_t type, uint32_t protection)
{
    (void)address; (void)type; (void)protection;
    uint32_t result = next_group; next_group += (size + 65535u) & ~65535u;
    assert(next_group < GROUPS + 0x7800000u); return result;
}
uint32_t void_ddraw_redirect_pixels(uint32_t surface, uint32_t pixels) { (void)surface; return pixels; }
int void_vm_readable(uint32_t address, uint32_t size)
{
    uint64_t end = (uint64_t)address + size;
    return (address >= 0xA00000u && end <= 0x1A00000u) ||
           (address >= WORLD && end <= WORLD + 0x1100000u) ||
           (address >= GROUPS && end <= GROUPS + 0x7800000u) ||
           (address >= HUD && end <= HUD + 0x200000u);
}
int void_vm_writable(uint32_t address, uint32_t size) { return void_vm_readable(address, size); }
int void_android_text_raster(const uint16_t *text, int count, int size, int weight, int italic,
    float scale, float dx, float dy, int width, int height, uint8_t *out)
{
    (void)text; (void)count; (void)size; (void)weight; (void)italic; (void)scale; (void)dx; (void)dy;
    memset(out, 255, (size_t)width * height); return 1;
}
static const uint16_t text[] = {0x0422, 0x0435, 0x043A, 0x0441, 0x0442};
static uint8_t mask[24 * 12];
static uint16_t *pixel(int group, int x, int y) { return raster_row(layers[group].guest, y) + x; }
static void fill(int group, int x, int y, int w, int h, uint16_t color)
{
    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c) *pixel(group, x + c, y + r) = color;
        stronghold_display_written(GVA(pixel(group, x, y + r)), (unsigned)w * 2);
    }
}
static void run(int group, int x, int y, uint32_t color)
{
    void_android_blend(layers[group].guest, 4056, 2076, RASTER_PITCH, 16, mask, 24, 12, 24,
        text, 5, -12, 400, 0, origin_x + x, origin_y + y, color, 0, 0);
}
static void run_hud(int x, int y)
{
    void_android_blend(HUD, W, H, HUD_PITCH, 16, mask, 24, 12, 24, text, 5, -12, 400, 0, x, y, 0xFFFFFF, 0, 0);
}
static void start(float scale)
{
    memset(GPTR(HUD), 0, HUD_PITCH * H);
    stronghold_display_set_ui_scale(scale); stronghold_original_layers();
    assert(frame_open && current == -1);
}
static void layer_text(int group, int x, int y)
{
    open_layer(group); assert(current == group);
    fill(group, x - 5, y - 5, 40, 25, 0x4208); run(group, x, y, 0x00FF00); close_layer();
}
static void seven_layers(void)
{
    start(1.2f);
    layer_text(0,200,H-160);layer_text(1,40,40);layer_text(1,40,H/2+30);
    layer_text(4,40,H-200);layer_text(3,W-120,40);layer_text(5,W/2,40);
    layer_text(6,W-120,H/2);layer_text(2,500,220);run_hud(300,H-80);
    assert(note_count==9&&notes[8]->kind==SH_UI_LAYERS);
    assert(notes[4]->kind==3&&notes[4]->target==3);
    stronghold_original_present();
    uint16_t *pixels[SH_UI_LAYERS];uint8_t *coverage[SH_UI_LAYERS];int bounds[SH_UI_LAYERS][4],w,h;
    assert(stronghold_display_text_layers(pixels,coverage,bounds,&w,&h)&&w==W&&h==H&&pixels[6]);
    int hidden[SH_UI_LAYERS][4];
    assert(stronghold_text_overlay_hide_layers(pixels,coverage,bounds,W,W,H,hidden)==9);
    assert(queues[0].count==2&&queues[1].count==2);
    for(int i=2;i<SH_UI_LAYERS;i++)assert(queues[i].count==1);
    assert(queues[SH_UI_LAYERS].count==0&&hidden[3][0]==W-118);
    assert(note_count==1&&notes[0]->kind==SH_UI_LAYERS);
    assert(*pixel(3,W-116,43)==0x07E0&&pixels[3][43*W+W-116]==0x4208);
    float x,y,scale;
    assert(stronghold_display_layer_view(3,W-120,40,&x,&y,&scale));
    assert(fabsf(x-(W-120*1.2f)*2)<.01f&&fabsf(y-96)<.01f&&fabsf(scale-2.4f)<.01f);
    assert(stronghold_display_layer_view(1,40,H/2+30,&x,&y,&scale));
    assert(fabsf(y-(H/2+30)*1.2f*2)<.01f);
    assert(stronghold_display_layer_view(4,40,H-200,&x,&y,&scale));
    assert(fabsf(y-(H-200*1.2f)*2)<.01f);
    assert(stronghold_display_layer_view(5,W/2,40,&x,&y,&scale)&&fabsf(x-W)<.01f&&fabsf(y-96)<.01f);
    assert(stronghold_display_layer_view(6,W-120,H/2,&x,&y,&scale)&&fabsf(y-H)<.01f);
    assert(!stronghold_display_layer_view(SH_UI_LAYERS,0,0,&x,&y,&scale));
    puts("HD crisp: seven real capture groups, explicit anchors across old half split, HUD kind7, nine Cyrillic runs hidden while guest raster stays PASS");
}

static void occlusion(void)
{
    start(1.f); layer_text(3, 600, 220); layer_text(2, 600, 220);
    stronghold_original_present(); stronghold_text_overlay_publish();
    assert(queues[3].count == 0 && queues[2].count == 1);
    puts("HD crisp ES2: right-overlay text below modal keeps raster; top dialog text is sharpened in layer order 0,1,4,3,5,6,2 PASS");
}
static void menu_and_disable(void)
{
    frame_ready = 0; memset(GPTR(HUD), 0, HUD_PITCH * H); run_hud(50, 50);
    assert(note_count == 1 && notes[0]->kind == SH_UI_LAYERS);
    uint16_t before = *(uint16_t *)GPTR(HUD + HUD_PITCH * 53 + 54 * 2);
    assert(before == 0xFFFF);
    assert(stronghold_text_overlay_hide_frame(GPTR(HUD), HUD_PITCH / 2, W, H) == 1);
    assert(queues[SH_UI_LAYERS].count == 1 && queues[3].count == 0);
    assert(!*(uint16_t *)GPTR(HUD + HUD_PITCH * 53 + 54 * 2));
    stronghold_text_overlay_restore_frame();
    assert(*(uint16_t *)GPTR(HUD + HUD_PITCH * 53 + 54 * 2) == before);
    stronghold_display_set_filters(3, 3, .25f, 1, 0);
    int enabled = 1; stronghold_display_filters(NULL, NULL, NULL, NULL, &enabled);
    assert(!enabled && !stronghold_display_crisp_text());
    assert(!stronghold_text_overlay_hide_frame(GPTR(HUD), HUD_PITCH / 2, W, H));
    for (int g = 0; g <= SH_UI_LAYERS; ++g) assert(!queues[g].count);
    assert(!note_count && *(uint16_t *)GPTR(HUD + HUD_PITCH * 53 + 54 * 2) == before);
    stronghold_display_set_filters(3, 3, .25f, 0, 1);
    stronghold_display_set_image_filter(1, .5f);
    assert(stronghold_display_crisp_text());
    puts("HD crisp: live menu dimensions/padded pitch, hide+restore, independent frame queue7 and setting disable/legacy filter retention PASS");
}
static void gl_last_layer(void)
{
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY); assert(eglInitialize(display, NULL, NULL));
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint n; assert(eglChooseConfig(display, attrs, &config, 1, &n) && n);
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE}, pa[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ca);
    EGLSurface surface = eglCreatePbufferSurface(display, config, pa);
    assert(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context));
    stronghold_display_surface_size(W, H); start(1.f); layer_text(6, W - 120, H/2);
    stronghold_original_present(); stronghold_text_overlay_publish(); assert(queues[6].count == 1);
    quad_t q = queues[6].quads[0];
    glViewport(0, 0, W, H); glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
    stronghold_text_overlay_draw(6, W, H); assert(glGetError() == GL_NO_ERROR);
    uint8_t rgba[4]; glReadPixels(q.x0 + 2, H - 1 - (q.y0 + 2), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    assert(rgba[0] == 0 && rgba[1] == 255 && rgba[2] == 0 && rgba[3] == 255);
    stronghold_text_overlay_reset_gl();
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface); eglDestroyContext(display, context); eglTerminate(display);
    puts("HD crisp GL: seventh-layer glyph queue really draws into EGL pbuffer PASS");
}
static void save_progress(void)
{
    start(1.2f);open_layer(0);fill(0,850,H-170,40,30,0xF800);close_layer();
    stronghold_original_present();MEM32(DIALOG)=14;
    MEM32(0x1503CC0u)=350;MEM32(0x1503CC4u)=220;
    MEM32(0x1503CC8u)=320;MEM32(0x1503CCCu)=120;
    for(int y=220;y<340;y++)for(int x=350;x<670;x++)raster_row(WORLD,y)[x]=0x07E0;
    stronghold_original_progress(0);
    assert(frame_ready&&!frame_taken&&published_used[2]&&!published_used[1]);
    assert(layers[0].pixels[(H-160)*W+860]==0xF800);
    assert(layers[2].pixels[250*W+400]==0x07E0&&layers[2].coverage[250*W+400]==255);
    MEM32(0xBC734C)=360;MEM32(0xBC7350)=300;
    for(int y=300;y<316;y++)for(int x=360;x<661;x++)raster_row(WORLD,y)[x]=0x001F;
    stronghold_original_progress(1);
    assert(layers[2].pixels[305*W+400]==0x001F&&layers[2].pixels[250*W+400]==0x07E0);
    assert(layers[0].pixels[(H-160)*W+860]==0xF800&&fabsf(published_scale-1.2f)<.001f);
    MEM32(DIALOG)=UINT32_MAX;
    puts("HD save progress: initial window, nonzero fill, complete scribe and stable scale PASS");
}
static void cart_scale(void)
{
    for(int percent=100;percent<=200;percent+=20)for(int x=0;x<W;x+=80) {
        start(percent/100.f);open_layer(1);
        fill(1,40,3,210,100,0x07E0);
        stronghold_original_resource_animation(1);
        fill(1,x,4,40,20,0xF800);
        stronghold_original_resource_animation(0);close_layer();
        stronghold_original_present();
        assert(top_fit.left==40&&top_fit.top==3&&top_fit.right==250&&top_fit.bottom==103);
        assert(layers[1].coverage[4*W+x]==255&&layers[1].pixels[4*W+x]==0xF800);
        assert(fabsf(published_scale-fminf(percent/100.f,W/960.f))<.001f);
        assert(published_edge_anchor[1][0]==0&&published_edge_anchor[1][1]==0);
    }
    start(1.2f);assert(!top_fit.right&&!resource_animation);
    puts("HD startup: real cart capture retained across 96 frames, functional resource bounds and UI100..200 scale/anchor stable PASS");
}
int main(void)
{
    const uint32_t memory[][2] = {{0xA00000, 0x1000000}, {WORLD, 0x1100000}, {GROUPS, 0x7800000}, {HUD, 0x200000}};
    for (unsigned i = 0; i < sizeof memory / sizeof memory[0]; ++i)
        assert(mmap(GPTR(memory[i][0]), memory[i][1], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    MEM32(SCREEN_WIDTH) = W; MEM32(SCREEN_HEIGHT) = H; MEM32(UI_PITCH) = HUD_PITCH; MEM32(UI_PIXELS) = HUD;
    MEM32(WORLD_PIXELS) = WORLD; MEM32(WORLD_SURFACE) = 1; MEM32(PRESENT_MODE) = 1;
    MEM32(RASTER_ORIGIN) = 8; MEM32(RASTER_ORIGIN + 4) = 4; MEM16(UI_KEY) = 0xF81F;
    for (int y = 3; y < 10; ++y) for (int x = 2; x < 22; ++x) mask[y * 24 + x] = x % 4 ? 128 : 255;
    stronghold_display_surface_size(2 * W, 2 * H);
    int x, y;
    assert(stronghold_display_locate(HUD + HUD_PITCH * 7 + 22, &x, &y) == SH_UI_LAYERS && x == 11 && y == 7);
    assert(stronghold_display_locate(HUD + HUD_PITCH * H, &x, &y) == -1);
    seven_layers(); occlusion(); menu_and_disable(); gl_last_layer();save_progress();cart_scale();
    puts("Stronghold HD crisp-text selfcheck PASS"); return 0;
}
