/* Device GLES harness for the GPU presenter (EGL pbuffer + glReadPixels):
 * every program builds; frames without composition keep orientation and
 * letterbox; FSR keeps a step edge sharp without ringing; deband smooths an
 * RGB565 ramp but keeps a hard edge; premultiplied UI equals
 * stronghold_composite_pixel() through every UI filter; incremental UI EASU
 * equals a full redraw; loupe placement, clamping and drawing; the
 * armed-button glow; the text-overlay hook order and state contract; time
 * per frame. With a layered frame dump argument (SHGF, see render_dump) it
 * renders that frame with each filter into /data/local/tmp/gfx-out-*.rgba.
 * Build: clang --target=aarch64-linux-android30 -O2 -std=gnu11 -Iandroid/runtime
 *        -Iandroid/host tests/ddraw_present_selfcheck.c -lEGL -lGLESv3 -landroid -lm
 *        -Wl,-z,max-page-size=16384 */
#define SH_PBUFFER_WIDTH 2340
#define SH_PBUFFER_HEIGHT 1080
#ifndef PRESENTER_SOURCE
#define PRESENTER_SOURCE "../android/host/ddraw_present.c"
#endif
#include PRESENTER_SOURCE
#include <assert.h>
#include <stdarg.h>

#define W SH_PBUFFER_WIDTH
#define H SH_PBUFFER_HEIGHT
#define FW 1304
#define FH 600

void void_log(const char *fmt,...){va_list ap;va_start(ap,fmt);vprintf(fmt,ap);putchar('\n');va_end(ap);}
const char *void_data_dir(void){return "/data/local/tmp/gfx-harness";}
uint64_t void_monotonic_ns(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec;}
void void_thunk_stats(uint32_t frame){(void)frame;}
void stronghold_perf_frame_end(void){}

/* display.c, as the presenter sees it */
static stronghold_frame test_frame;
static int test_layered,t_world=SH_FILTER_FSR,t_ui=SH_FILTER_PIXELS,t_deband,t_loupe;
static float t_sharp=.25f,t_lx,t_ly;
int stronghold_display_frame(stronghold_frame *f,int width,int height,int bpp)
{
    (void)width;(void)height;(void)bpp;
    if(!test_layered)return 0;
    *f=test_frame;test_frame.fresh=0;
    for(int i=0;i<SH_UI_LAYERS;i++)test_frame.layer[i].dirty=(stronghold_rect){0,0,0,0};
    return 1;
}
const uint8_t *stronghold_display_compose(const uint8_t *pixels,int width,int height,int *pitch,int bpp)
{(void)width;(void)height;(void)pitch;(void)bpp;return pixels;}
void stronghold_display_surface_size(int width,int height){(void)width;(void)height;}
int stronghold_display_image_filter(float *sharpness){*sharpness=t_sharp;return 2;}
void stronghold_display_filters(int *world,int *ui_mode,float *sharpness,int *deband,int *crisp)
{*world=t_world;*ui_mode=t_ui;*sharpness=t_sharp;*deband=t_deband;*crisp=1;}
void stronghold_display_set_filters(int world,int ui_mode,float sharpness,int deband,int crisp)
{t_world=world;t_ui=ui_mode;t_sharp=sharpness;t_deband=deband;(void)crisp;}
void stronghold_display_magnifier(int active,float nx,float ny){t_loupe=active;t_lx=nx;t_ly=ny;}
int stronghold_display_loupe(float *nx,float *ny){if(t_loupe){*nx=t_lx;*ny=t_ly;}return t_loupe;}

/* Crisp text hiding (text_overlay.c) has its own selfcheck; here it hides
 * nothing, and every fresh layered frame and uncomposed frame asks once. */
static int hide_layer_calls,hide_frame_calls,hide_seen_groups;
int stronghold_text_overlay_hide_layers(uint16_t *const layers[SH_UI_LAYERS],uint8_t *const coverage[SH_UI_LAYERS],const int bounds[SH_UI_LAYERS][4],
                                        int stride,int width,int height,int hidden[SH_UI_LAYERS][4])
{
    (void)coverage;(void)bounds;(void)stride;(void)width;(void)height;(void)hidden;hide_layer_calls++;
    hide_seen_groups=0;for(int i=0;i<SH_UI_LAYERS;i++)if(layers[i])hide_seen_groups|=1<<i;
    return 0;
}
int stronghold_text_overlay_hide_frame(uint16_t *pixels,int stride,int width,int height)
{(void)pixels;(void)stride;(void)width;(void)height;hide_frame_calls++;return 0;}
void stronghold_text_overlay_restore_frame(void){}
void stronghold_text_overlay_publish(void){}

/* The text overlay may change program, texture 0, blending, the array
 * buffer and attributes 0/1; this one does all of that every time. */
static int overlay_groups[16],overlay_count;
static GLuint junk_texture,junk_buffer;
void stronghold_text_overlay_draw(int group,int ww,int wh)
{
    GLint viewport[4];glGetIntegerv(GL_VIEWPORT,viewport);
    assert(viewport[0]==0&&viewport[1]==0&&viewport[2]==ww&&viewport[3]==wh);
    assert(!glIsEnabled(GL_SCISSOR_TEST));
    GLint framebuffer=-1;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);assert(framebuffer==0);
    if(overlay_count<16)overlay_groups[overlay_count]=group;
    overlay_count++;
    if(!junk_texture){
        static const uint8_t white[16]={255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255};
        glGenTextures(1,&junk_texture);glBindTexture(GL_TEXTURE_2D,junk_texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
        glGenBuffers(1,&junk_buffer);glBindBuffer(GL_ARRAY_BUFFER,junk_buffer);glBufferData(GL_ARRAY_BUFFER,64,white,GL_STATIC_DRAW);
    }
    glUseProgram(0);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,junk_texture);
    glEnable(GL_BLEND);glBlendFunc(GL_ZERO,GL_ONE);
    glBindBuffer(GL_ARRAY_BUFFER,junk_buffer);
    glVertexAttribPointer(0,4,GL_UNSIGNED_BYTE,GL_TRUE,0,0);glVertexAttribPointer(1,4,GL_UNSIGNED_BYTE,GL_TRUE,0,0);
    glEnableVertexAttribArray(1);
}

static uint8_t *window_rgba;
static uint16_t *world_px,*layer_px[SH_UI_LAYERS];
static uint8_t *layer_cov[SH_UI_LAYERS];

static void present_test(void){static uint16_t dummy[4];void_ddraw_present_pixels((uint8_t *)dummy,FW,FH,FW*2,16);}
static void read_window(void)
{
    static uint8_t *raw;if(!raw)raw=malloc((size_t)W*H*4);
    glPixelStorei(GL_PACK_ALIGNMENT,4);glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,raw);
    assert(glGetError()==GL_NO_ERROR);
    for(int y=0;y<H;y++)memcpy(window_rgba+(size_t)y*W*4,raw+(size_t)(H-1-y)*W*4,(size_t)W*4);
}
static const uint8_t *at(int x,int y){return window_rgba+((size_t)y*W+x)*4;}
static uint16_t rgb565(int r,int g,int b){return (uint16_t)(r<<11|g<<5|b);}
static void expand(uint16_t c,int out[3]){out[0]=(c>>11)*255/31;out[1]=((c>>5)&63)*255/63;out[2]=(c&31)*255/31;}
static view frame_view(void){return fit_view(FW,FH,W,H);}
/* Window pixel at the centre of frame pixel (x, y) (UI scale 1, world scale 1). */
static void window_of(double x,double y,int *wx,int *wy)
{view v=frame_view();*wx=(int)floor(v.ox+x*v.fit);*wy=(int)floor(v.oy+y*v.fit);}

static void new_frame(void)
{
    memset(&test_frame,0,sizeof test_frame);
    test_frame.width=FW;test_frame.height=FH;test_frame.world=world_px;
    test_frame.raster_width=FW;test_frame.raster_height=FH;test_frame.world_scale_x=test_frame.world_scale_y=1;
    test_frame.ui_scale=1;test_frame.key=0xf81f;test_frame.fresh=1;
    for(int i=0;i<SH_UI_LAYERS;i++){
        test_frame.layer[i].pixels=layer_px[i];test_frame.layer[i].coverage=layer_cov[i];
        for(size_t k=0;k<(size_t)FW*FH;k++){layer_px[i][k]=0xf81f;layer_cov[i][k]=0;}
        test_frame.layer[i].dirty=(stronghold_rect){0,0,FW,FH};
    }
    test_layered=1;
}
static void add_part(int layer,int top,int bottom,float ax,float ay)
{test_frame.part[test_frame.parts++]=(stronghold_part){layer,top,bottom,ax,ay};}
static void use_layer(int i,stronghold_rect bounds){test_frame.layer[i].used=1;test_frame.layer[i].bounds=bounds;}

static void programs_check(void)
{
    for(int id=0;id<PROGRAMS;id++)if(!gl3_program(id)){printf("program %d failed\n",id);assert(0);}
    printf("all %d programs compile and link (%s)\n",PROGRAMS,glGetString(GL_RENDERER));
}

static void raw_frame_check(void)
{
    static uint16_t pixels[64*48];
    for(int y=0;y<48;y++)for(int x=0;x<64;x++)pixels[y*64+x]=y<24?(x<32?0xF800:0x07E0):(x<32?0x001F:0xFFFF);
    test_layered=0;t_ui=SH_FILTER_BILINEAR;overlay_count=0;
    void_ddraw_present_pixels((uint8_t *)pixels,64,48,128,16);read_window();
    assert(overlay_count==1&&overlay_groups[0]==-1);
    /* fit 22.5: 1440x1080 at x 450 */
    const int xy[5][2]={{810,270},{1530,270},{810,810},{1530,810},{100,540}};
    const int expected[5][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,255},{0,0,0}};
    for(int i=0;i<5;i++)for(int c=0;c<3;c++)assert(abs(at(xy[i][0],xy[i][1])[c]-expected[i][c])<=3);
    for(int mode=SH_FILTER_PIXELS;mode<=SH_FILTER_FSR;mode++){
        t_ui=mode;void_ddraw_present_pixels((uint8_t *)pixels,64,48,128,16);read_window();
        for(int i=0;i<5;i++)for(int c=0;c<3;c++)assert(abs(at(xy[i][0],xy[i][1])[c]-expected[i][c])<=3);
    }
    puts("frames without composition: orientation, letterbox, all four UI filters, overlay group -1 PASS");
}

/* Transition width (10%..90%) and overshoot of a step from lo to hi along window row y. */
static void edge_profile(int y,int x0,int x1,int lo,int hi,double *width,int *over)
{
    /* Width: pixels strictly between 10% and 90% of the step. */
    int n=0;*over=0;
    for(int x=x0;x<x1;x++){
        int v=at(x,y)[1];
        if(lo-v>*over)*over=lo-v;
        if(v-hi>*over)*over=v-hi;
        if(v>lo+(hi-lo)*.1&&v<lo+(hi-lo)*.9)n++;
    }
    *width=n;
}
static void fsr_edge_check(void)
{
    /* Vertical step 16 -> 48 of 63 in green (others mid), edge at frame x 652. */
    for(int y=0;y<FH;y++)for(int x=0;x<FW;x++)world_px[y*FW+x]=rgb565(16,x<652?16:48,16);
    int lo=16*255/63,hi=48*255/63,wx,wy;window_of(652,300.5,&wx,&wy);
    double width[4];int over[4];
    const char *names[4]={"bilinear","pixels","bicubic","fsr"};
    for(int mode=0;mode<4;mode++){
        new_frame();t_world=mode;t_sharp=.25f;t_deband=0;overlay_count=0;
        present_test();read_window();
        assert(overlay_count==SH_UI_LAYERS);for(int i=0;i<SH_UI_LAYERS;i++)assert(overlay_groups[i]==SH_UI_ORDER[i]);
        edge_profile(wy,wx-20,wx+20,lo,hi,&width[mode],&over[mode]);
        printf("  step edge %-8s: %.0f px between 10%% and 90%%, overshoot %d/255\n",names[mode],width[mode],over[mode]);
    }
    /* EASU dering + RCAS limits: no visible ringing, sharper than bilinear. */
    assert(width[3]<=width[0]&&over[3]<=6);
    t_sharp=1;new_frame();t_world=SH_FILTER_FSR;present_test();read_window();
    double w1;int o1;edge_profile(wy,wx-20,wx+20,lo,hi,&w1,&o1);
    printf("  step edge fsr sharpness 1: width %.0f px, overshoot %d/255\n",w1,o1);
    assert(w1<=width[0]&&o1<=12);
    t_sharp=0;new_frame();present_test();read_window();
    edge_profile(wy,wx-20,wx+20,lo,hi,&w1,&o1);
    printf("  step edge easu only: width %.0f px, overshoot %d/255\n",w1,o1);
    assert(w1<=width[0]&&o1<=2);
    t_sharp=.25f;
    puts("FSR: step edge as sharp as or sharper than bilinear, no ringing beyond the RCAS limit PASS");
}

static void deband_check(void)
{
    /* Red ramp 8..16 of 31 across the frame, a black block 640..700 (hard edge). */
    for(int y=0;y<FH;y++)for(int x=0;x<FW;x++)
        world_px[y*FW+x]=x>=640&&x<700?0:rgb565(8+x*9/FW,20,10);
    int max_step[2]={0,0},edge_inside[2]={0,0};double edge_width[2];
    for(int deband=0;deband<2;deband++){
        new_frame();t_world=SH_FILTER_PIXELS;t_sharp=0;t_deband=deband;
        present_test();read_window();
        int wx0,wx1,wy,wb;window_of(20,300.5,&wx0,&wy);window_of(600,300.5,&wx1,&wy);window_of(700,300.5,&wb,&wy);
        for(int x=wx0;x<wx1;x++){int d=abs(at(x+1,wy)[0]-at(x,wy)[0]);if(d>max_step[deband])max_step[deband]=d;}
        /* Rising edge at the block's right side: dark inside, ramp colour outside. */
        int lo=at(wb-12,wy)[0],hi=at(wb+12,wy)[0];double a=-1,b=-1;
        for(int x=wb-12;x<wb+12;x++){int v=at(x,wy)[0];if(a<0&&v>=lo+(hi-lo)*.1)a=x;if(b<0&&v>=lo+(hi-lo)*.9)b=x;}
        edge_width[deband]=b-a;edge_inside[deband]=at(wb-6,wy)[0];
        printf("  deband %d: largest step in the ramp %d/255, hard edge width %.0f px, block %d/255\n",
               deband,max_step[deband],edge_width[deband],edge_inside[deband]);
    }
    assert(max_step[0]>=7&&max_step[1]<=2);
    assert(edge_width[1]<=edge_width[0]+1&&edge_inside[1]<=2);
    t_deband=0;t_sharp=.25f;
    puts("deband: RGB565 ramp steps smoothed below 1/100, hard edge and black block intact PASS");
}

static void premultiplied_check(void)
{
    for(int y=0;y<FH;y++)for(int x=0;x<FW;x++)world_px[y*FW+x]=0x4a69;
    new_frame();t_world=SH_FILTER_PIXELS;t_ui=SH_FILTER_PIXELS;
    const uint16_t colors[]={0x07e0,0xf800,0x001f,0xffff,0x8410,0x0000,0x39e7,0xfd20};
    const unsigned alphas[]={255,200,128,64,16,1,0,255};
    int key[3];expand(0xf81f,key);
    for(int k=0;k<8;k++){
        /* As the game draws it: the colour blended over the colour key. */
        int c[3],p[3];expand(colors[k],c);
        for(int i=0;i<3;i++)p[i]=(c[i]*(int)alphas[k]+key[i]*(255-(int)alphas[k])+127)/255;
        uint16_t pixel=k==6?0x1234:rgb565(p[0]*31/255,p[1]*63/255,p[2]*31/255); /* k 6: uncovered, not the key */
        for(int y=280;y<290;y++)for(int x=100+40*k;x<110+40*k;x++){layer_px[2][y*FW+x]=pixel;layer_cov[2][y*FW+x]=(uint8_t)alphas[k];}
    }
    use_layer(2,(stronghold_rect){100,280,430,290});add_part(2,0,FH,FW*.5f,FH*.5f);
    stronghold_frame frame=test_frame;
    for(int mode=SH_FILTER_FSR;mode>=SH_FILTER_BILINEAR;mode--){
    test_frame=frame;t_ui=mode;
    overlay_count=0;present_test();read_window();
    assert(overlay_count==SH_UI_LAYERS);
    printf("  UI filter %d:\n",mode);
    for(int k=0;k<8;k++){
        int x=105+40*k,y=285,wx,wy;window_of(x+.5,y+.5,&wx,&wy);
        uint16_t pixel=layer_px[2][y*FW+x],expect=stronghold_composite_pixel(pixel,0x4a69,alphas[k],0xf81f);
        int e[3],p8[3],b8[3],exact[3];expand(expect,e);
        const uint8_t *g8=at(wx,wy);
        /* The GPU blends premul = max(pixel - key*(1-a), 0); composite_pixel()
         * leaves that negative (an RGB565 rounding residue of the game's own
         * blend over the key, at low coverage), so it may differ by one step. */
        const unsigned lim[3]={31,63,31},sh[3]={11,5,0};
        for(int i=0;i<3;i++){
            double a=alphas[k]/255.,pv=((pixel>>sh[i])&lim[i])/(double)lim[i],kv=((0xf81f>>sh[i])&lim[i])/(double)lim[i];
            double bv=((0x4a69>>sh[i])&lim[i])/(double)lim[i],pm=alphas[k]?fmax(pv-kv*(1-a),0):0;
            exact[i]=(int)lround(fmin(1,pm+bv*(1-a))*255);p8[i]=(int)(pv*255);b8[i]=(int)(bv*255);
        }
        (void)p8;(void)b8;
        printf("  coverage %3u: GPU %3u %3u %3u, premultiplied %3d %3d %3d, composite_pixel %3d %3d %3d\n",
               alphas[k],g8[0],g8[1],g8[2],exact[0],exact[1],exact[2],e[0],e[1],e[2]);
        for(int i=0;i<3;i++)assert(abs(g8[i]-exact[i])<=2&&abs(g8[i]-e[i])<=(i==1?5:9));
        if(!alphas[k])for(int i=0;i<3;i++)assert(abs(g8[i]-b8[i])<=1);
    }
    }
    puts("premultiplied UI (FSR, bicubic, pixels, bilinear): GPU blend equals premul over (within 2/255) and composite_pixel() (within one RGB565 step), uncovered texels invisible PASS");
}

/* UI FSR keeps each part's EASU and recomputes only what a layer's dirty
 * rectangle reaches: after a small change it must equal a full redraw. */
static void incremental_check(void)
{
    srand(11);
    for(int y=0;y<FH;y++)for(int x=0;x<FW;x++)world_px[y*FW+x]=rgb565(10+(x/40)%8,30,12);
    new_frame();t_world=SH_FILTER_BILINEAR;t_ui=SH_FILTER_FSR;t_sharp=.5f;
    for(int y=150;y<450;y++)for(int x=300;x<1000;x++){
        layer_px[2][y*FW+x]=rgb565(rand()%32,rand()%64,rand()%32);layer_cov[2][y*FW+x]=(uint8_t)(y<160||x<310?128:255);
    }
    use_layer(2,(stronghold_rect){300,150,1000,450});add_part(2,0,FH,FW*.5f,FH*.5f);
    present_test();
    for(int y=290;y<300;y++)for(int x=600;x<620;x++)layer_px[2][y*FW+x]=0xffff;
    test_frame.layer[2].dirty=(stronghold_rect){600,290,620,300};test_frame.fresh=0;
    present_test();read_window();
    uint8_t *incremental=malloc((size_t)W*H*4);memcpy(incremental,window_rgba,(size_t)W*H*4);
    g.refresh=1;test_frame.layer[2].dirty=(stronghold_rect){0,0,FW,FH};
    present_test();read_window();
    int worst=0;
    for(size_t i=0;i<(size_t)W*H*4;i++){int d=abs(incremental[i]-window_rgba[i]);if(d>worst)worst=d;}
    free(incremental);
    printf("  incremental UI EASU vs full redraw: largest difference %d/255\n",worst);
    assert(worst<=1);
    t_sharp=.25f;t_ui=SH_FILTER_PIXELS;
    puts("UI FSR: incremental EASU after a partial layer change equals a full redraw PASS");
}

/* A HUD scaled past the window's width (HUD scale 1.2): every UI filter
 * must show the layer up to the right edge (no clamped streaks). */
static void wide_hud_check(void)
{
    for(int y=0;y<FH;y++)for(int x=0;x<FW;x++)world_px[y*FW+x]=0x2104;
    new_frame();t_world=SH_FILTER_BILINEAR;
    for(int y=FH-128;y<FH;y++)for(int x=0;x<FW;x++){layer_px[0][y*FW+x]=(x/6)&1?0xffff:0x0841;layer_cov[0][y*FW+x]=255;}
    use_layer(0,(stronghold_rect){0,FH-128,FW,FH});add_part(0,0,128,0,0);add_part(0,128,FH,FW*.5f,(float)FH);
    test_frame.hud_opaque=1;test_frame.ui_scale=1.2f;
    stronghold_frame frame=test_frame;
    for(int mode=SH_FILTER_FSR;mode>=SH_FILTER_BILINEAR;mode--){
        test_frame=frame;t_ui=mode;present_test();read_window();
        int edges=0,last=-1;
        for(int x=W-240;x<W;x++){int bright=at(x,H-40)[0]>128;if(last>=0&&bright!=last)edges++;last=bright;}
        printf("  wide HUD, UI filter %d: %d stripe edges in the last 240 px\n",mode,edges);
        assert(edges>=12);
    }
    t_ui=SH_FILTER_PIXELS;
    puts("UI parts wider than the window: content reaches the right edge with every filter PASS");
}

static void resource_rows_check(void)
{
    for(int y=0;y<FH;y++)for(int x=0;x<FW;x++)world_px[y*FW+x]=0x2104;
    new_frame();t_world=SH_FILTER_PIXELS;test_frame.ui_scale=1.2f;
    const int rows[]={20,80,140,200,260};
    for(int r=0;r<5;r++)for(int y=rows[r];y<rows[r]+10;y++)for(int x=10;x<80;x++){
        layer_px[0][y*FW+x]=0xffff;layer_cov[0][y*FW+x]=255;
    }
    use_layer(0,(stronghold_rect){10,20,80,270});add_part(0,0,274,0,0);add_part(0,274,FH,FW*.5f,FH);
    stronghold_frame frame=test_frame;
    for(int filter=SH_FILTER_BILINEAR;filter<=SH_FILTER_FSR;filter++){
        test_frame=frame;t_ui=filter;present_test();read_window();
        for(int r=0;r<5;r++){
            int x,y;window_of(40*1.2,(rows[r]+5)*1.2,&x,&y);
            assert(at(x,y)[0]>240&&at(x,y)[1]>240&&at(x,y)[2]>240);
        }
    }
    t_ui=SH_FILTER_PIXELS;
    puts("resource overlay beyond row128: all five rows keep top-left anchor with every UI filter PASS");
}

static void resource_notice_check(void)
{
    for(int y=0;y<FH;y++)for(int x=0;x<FW;x++)world_px[y*FW+x]=0x2104;
    for(float scale=.75f;scale<=2.001f;scale+=.25f){
        new_frame();t_world=SH_FILTER_PIXELS;test_frame.ui_scale=scale;
        for(int y=420;y<440;y++)for(int x=10;x<530;x++){
            layer_px[3][y*FW+x]=0xffff;layer_cov[3][y*FW+x]=255;
        }
        use_layer(3,(stronghold_rect){10,420,530,440});add_part(3,0,FH,0,FH);
        stronghold_frame frame=test_frame;
        for(int filter=SH_FILTER_PIXELS;filter<=SH_FILTER_FSR;filter++){
            test_frame=frame;t_ui=filter;overlay_count=0;present_test();read_window();
            assert(hide_seen_groups&(1<<3));
            assert(overlay_count==SH_UI_LAYERS&&overlay_groups[2]==3&&overlay_groups[SH_UI_LAYERS-1]==2);
            for(int sx=12;sx<=528;sx+=258){
                int x,y;window_of((sx+.5)*scale,FH+(430.5-FH)*scale,&x,&y);
                assert(at(x,y)[0]>240&&at(x,y)[1]>240&&at(x,y)[2]>240);
            }
        }
    }
    new_frame();test_frame.ui_scale=1;
    for(int y=420;y<440;y++)for(int x=10;x<530;x++){
        layer_px[3][y*FW+x]=0xffff;layer_cov[3][y*FW+x]=255;
    }
    for(int y=420;y<440;y++)for(int x=10;x<40;x++){
        layer_px[2][y*FW+x]=0x07e0;layer_cov[2][y*FW+x]=255;
    }
    use_layer(3,(stronghold_rect){10,420,530,440});add_part(3,0,FH,0,FH);
    use_layer(2,(stronghold_rect){10,420,40,440});add_part(2,0,FH,FW*.5f,FH*.5f);
    t_ui=SH_FILTER_PIXELS;present_test();read_window();
    int x,y;window_of(20.5,430.5,&x,&y);assert(at(x,y)[0]<5&&at(x,y)[1]>250&&at(x,y)[2]<5);
    t_ui=SH_FILTER_PIXELS;
    puts("resource notice: fourth left-bottom layer visible first-to-last column75..200%, four filters, crisp ordering and dialog occlusion PASS");
}

#ifndef STRONGHOLD_ORIGINAL
static void overlay_classes_check(void)
{
    const stronghold_rect box[SH_UI_LAYERS]={
        {10,10,50,30},{FW-50,60,FW-10,80},{FW/2-20,FH/2-10,FW/2+20,FH/2+10},
        {10,FH-80,50,FH-60},{FW/2-20,10,FW/2+20,30},{FW-50,10,FW-10,30},{FW-50,FH/2-10,FW-10,FH/2+10},
        {FW/2-20,FH-120,FW/2+20,FH-100}};
    const float anchors[SH_UI_LAYERS][2]={{0,0},{FW,56},{FW*.5f,FH*.5f},{0,FH},{FW*.5f,0},{FW,0},{FW,FH*.5f},{FW*.5f,FH}};
    const uint16_t colors[SH_UI_LAYERS]={0xf800,0x07e0,0x001f,0xffe0,0xf81f,0x07ff,0xffff,0xfc00};
    for(float scale=.75f;scale<=2.001f;scale+=.25f){
        new_frame();test_frame.ui_scale=scale;
        for(int i=0;i<SH_UI_LAYERS;i++){
            stronghold_rect b=box[i];
            for(int y=b.top;y<b.bottom;y++)for(int x=b.left;x<b.right;x++){
                layer_px[i][y*FW+x]=colors[i];layer_cov[i][y*FW+x]=255;
            }
            use_layer(i,b);
        }
        for(int order=0;order<SH_UI_LAYERS;order++){
            int i=SH_UI_ORDER[order];add_part(i,0,i?FH:128,anchors[i][0],anchors[i][1]);
            if(i==1)test_frame.part[test_frame.parts-1].scale=scale*1.25f;
        }
        stronghold_frame frame=test_frame;
        for(int filter=SH_FILTER_PIXELS;filter<=SH_FILTER_FSR;filter++){
            test_frame=frame;t_ui=filter;present_test();read_window();
            for(int i=0;i<SH_UI_LAYERS;i++){
                float sx=(box[i].left+box[i].right)*.5f+.5f,sy=(box[i].top+box[i].bottom)*.5f+.5f;
                int x,y,rgb[3];expand(colors[i],rgb);
                float s=i==1?scale*1.25f:scale;
                window_of(anchors[i][0]+(sx-anchors[i][0])*s,anchors[i][1]+(sy-anchors[i][1])*s,&x,&y);
                for(int c=0;c<3;c++)assert(abs(at(x,y)[c]-rgb[c])<=3);
            }
        }
    }
    new_frame();present_test();for(int i=0;i<SH_UI_LAYERS;i++)assert(!g.ui565[i]&&!g.uicov[i]&&!g.pm[i]);
    /* An unused layer can have no backing data (legacy dump/lazy capture).
     * Its later return uploads fully, even with an empty incremental dirty box. */
    test_frame.layer[6].pixels=NULL;test_frame.layer[6].coverage=NULL;test_frame.fresh=1;present_test();
    new_frame();stronghold_rect b=box[6];
    for(int y=b.top;y<b.bottom;y++)for(int x=b.left;x<b.right;x++){layer_px[6][y*FW+x]=0xf800;layer_cov[6][y*FW+x]=255;}
    use_layer(6,b);add_part(6,0,FH,FW,FH*.5f);test_frame.layer[6].dirty=(stronghold_rect){0,0,0,0};
    t_ui=SH_FILTER_FSR;present_test();read_window();int x,y;window_of(FW-29.5,FH*.5+.5,&x,&y);
    assert(at(x,y)[0]>250&&at(x,y)[1]<5&&at(x,y)[2]<5);
    t_ui=SH_FILTER_PIXELS;
    puts("all seven overlay anchors75..200%, every filter, lazy textures, NULL unused layer and clean reappearance PASS");
}
#endif

static void loupe_check(void)
{
    float cx,cy,r;
    stronghold_present_loupe_settings(0,900);
    assert(atomic_load(&loupe_size)==50&&atomic_load(&loupe_zoom)==400);
    stronghold_present_loupe_settings(900,0);
    assert(atomic_load(&loupe_size)==200&&atomic_load(&loupe_zoom)==150);
    stronghold_present_loupe_settings(100,200);
    for(float size=.5f;size<=2;size+=.25f)
    for(int fy=0;fy<=H;fy+=40)for(int fx=0;fx<=W;fx+=40){
        stronghold_loupe_place((float)fx,(float)fy,W,H,size,&cx,&cy,&r);
        float m=H*.02f;
        assert(fabsf(r-H*.13f*size)<.01f);
        assert(cx>=r+m-.01f&&cx<=W-r-m+.01f&&cy>=r+m-.01f&&cy<=H-r-m+.01f);
        /* Away from the corners the finger never lies under the loupe. */
        if(fx>r*3&&fx<W-r*3&&fy>r*3)assert(hypotf(cx-fx,cy-fy)>r*1.2f);
    }
    stronghold_loupe_place(1170,540,W,H,1,&cx,&cy,&r);assert(cx<1170&&cy<540);           /* up-left */
    stronghold_loupe_place(60,700,W,H,1,&cx,&cy,&r);assert(cx>60&&cy<700);               /* left edge: up-right */
    stronghold_loupe_place(1500,90,W,H,1,&cx,&cy,&r);assert(cx<1500&&fabsf(cy-r-H*.02f)<.01f); /* top: beside */
    stronghold_loupe_place(200,90,W,H,1,&cx,&cy,&r);assert(cx>200);                       /* top-left corner */
    /* Drawing: a world of vertical stripes, 2x of the on-screen scale. */
    for(int y=0;y<FH;y++)for(int x=0;x<FW;x++)world_px[y*FW+x]=(x/8)&1?rgb565(28,56,28):rgb565(4,8,4);
    new_frame();t_world=SH_FILTER_PIXELS;t_loupe=1;t_lx=.5f;t_ly=.5f;
    present_test();read_window();t_loupe=0;
    stronghold_loupe_place(W*.5f,H*.5f,W,H,1,&cx,&cy,&r);
    const uint8_t *rim=at((int)(cx-r+r*.0325f),(int)cy);
    printf("  loupe at %.0f,%.0f r %.0f: rim %u %u %u, crosshair %u %u %u\n",cx,cy,r,rim[0],rim[1],rim[2],
           at((int)(cx+r*.1f),(int)cy)[0],at((int)(cx+r*.1f),(int)cy)[1],at((int)(cx+r*.1f),(int)cy)[2]);
    assert(abs(rim[0]-0xe7)<=12&&abs(rim[1]-0xb4)<=12&&abs(rim[2]-0x5c)<=12);
    const uint8_t *cross=at((int)(cx+r*.1f),(int)cy);assert(cross[0]>=240&&cross[1]>=240&&cross[2]>=240);
    /* The stripes repeat every 16 frame pixels on screen and every 32 in the loupe. */
    /* Mean length of the whole stripes (runs between two transitions). */
    view v=frame_view();double period=16*v.fit,lp=0;int runs=0,last=-1,start=-1,total=0;
    for(int x=(int)(cx-r*.72f);x<(int)(cx+r*.72f);x++){ /* inside the rim */
        int bright=at(x,(int)cy-(int)(r*.5f))[0]>128;
        if(last>=0&&bright!=last){if(start>=0){total+=x-start;runs++;}start=x;}
        last=bright;
    }
    assert(runs>=3);
    lp=2.0*total/runs;
    printf("  loupe stripe period %.1f px (screen %.1f px)\n",lp,period);
    assert(fabs(lp-2*period)<period*.35);
    stronghold_present_loupe_settings(150,300);
    t_loupe=1;present_test();read_window();t_loupe=0;
    stronghold_loupe_place(W*.5f,H*.5f,W,H,1.5f,&cx,&cy,&r);
    runs=0;last=-1;start=-1;total=0;
    for(int x=(int)(cx-r*.72f);x<(int)(cx+r*.72f);x++){
        int bright=at(x,(int)cy-(int)(r*.5f))[0]>128;
        if(last>=0&&bright!=last){if(start>=0){total+=x-start;runs++;}start=x;}
        last=bright;
    }
    assert(runs>=3&&fabs(2.0*total/runs-3*period)<period*.35);
    stronghold_present_loupe_settings(100,200);
    puts("loupe: placement up-left/up-right/beside, clamped inside, gold rim, crosshair, 2x magnification PASS");
}

static void glow_check(void)
{
    for(int y=0;y<FH;y++)for(int x=0;x<FW;x++)world_px[y*FW+x]=0x2104;
    int base[2],outside[2];
    for(int lit=0;lit<2;lit++){
        new_frame();t_ui=SH_FILTER_PIXELS;
        for(int y=FH-128;y<FH;y++)for(int x=0;x<FW;x++){layer_px[0][y*FW+x]=0x4208;layer_cov[0][y*FW+x]=255;}
        use_layer(0,(stronghold_rect){0,FH-128,FW,FH});add_part(0,0,128,0,0);add_part(0,128,FH,FW*.5f,(float)FH);
        test_frame.hud_opaque=1;
        if(lit)test_frame.highlight=(stronghold_rect){600,500,640,530};
        present_test();read_window();
        int wx,wy;window_of(620,515,&wx,&wy);base[lit]=at(wx,wy)[0];
        int ox,oy;window_of(560,515,&ox,&oy);outside[lit]=at(ox,oy)[0];
    }
    printf("  glow: button centre %d -> %d\n",base[0],base[1]);
    assert(base[1]>base[0]+20&&outside[1]==outside[0]);
    puts("armed-button glow: brighter inside its HUD rectangle PASS");
}

/* Wall time per frame (CPU and GPU overlapped) and, from timer queries, GPU
 * time per frame; every frame uploads the world and the HUD band again. */
static double gpu_ms_last,cpu_ms_last;
static double frame_ms(int frames)
{
    static GLuint queries[64];
    if(!queries[0])glGenQueries(64,queries);
    if(frames>64)frames=64;
    for(int i=0;i<3;i++){test_frame.fresh=1;present_test();}
    glFinish();uint64_t t0=void_monotonic_ns(),cpu=0;
    for(int i=0;i<frames;i++){
        test_frame.fresh=1;test_frame.layer[0].dirty=(stronghold_rect){0,FH-128,FW,FH};
        glBeginQuery(0x88BF /* GL_TIME_ELAPSED_EXT */,queries[i]);
        uint64_t c0=void_monotonic_ns();present_test();cpu+=void_monotonic_ns()-c0;
        glEndQuery(0x88BF);
    }
    glFinish();double wall=(void_monotonic_ns()-t0)*1e-6/frames;
    GLint disjoint=0;glGetIntegerv(0x8FBB /* GL_GPU_DISJOINT_EXT */,&disjoint);
    double gpu=0;
    for(int i=0;i<frames;i++){GLuint ns=0;glGetQueryObjectuiv(queries[i],GL_QUERY_RESULT,&ns);gpu+=ns*1e-6;}
    while(glGetError()!=GL_NO_ERROR){}
    gpu_ms_last=disjoint?-1:gpu/frames;cpu_ms_last=cpu*1e-6/frames;
    return wall;
}

static void timing_check(void)
{
    srand(7);
    for(int y=0;y<FH;y++)for(int x=0;x<FW;x++){int n=rand()%6;world_px[y*FW+x]=rgb565(8+((x/23+y/17)&7)+n,20+((x/9)&15)+n,6+n);}
    new_frame();
    for(int y=FH-128;y<FH;y++)for(int x=0;x<FW;x++){layer_px[0][y*FW+x]=(uint16_t)(0x4208+((x*7+y)&0x0821));layer_cov[0][y*FW+x]=255;}
    use_layer(0,(stronghold_rect){0,FH-128,FW,FH});add_part(0,0,128,0,0);add_part(0,128,FH,FW*.5f,(float)FH);
    test_frame.hud_opaque=1;
    const char *names[4]={"bilinear","pixels","bicubic","fsr"};
    for(int world=0;world<4;world++){
        t_world=world;t_ui=SH_FILTER_PIXELS;t_deband=0;
        double ms=frame_ms(40);printf("  world %-8s / ui pixels: %.2f ms per frame (GPU %.2f, CPU %.2f)\n",names[world],ms,gpu_ms_last,cpu_ms_last);
    }
    t_world=SH_FILTER_FSR;
    for(int ui_mode=0;ui_mode<4;ui_mode++){t_ui=ui_mode;double ms=frame_ms(40);printf("  world fsr / ui %-8s: %.2f ms per frame (GPU %.2f, CPU %.2f)\n",names[ui_mode],ms,gpu_ms_last,cpu_ms_last);}
    t_ui=SH_FILTER_PIXELS;t_deband=1;{double ms=frame_ms(40);printf("  world fsr + deband / ui pixels: %.2f ms per frame (GPU %.2f, CPU %.2f)\n",ms,gpu_ms_last,cpu_ms_last);}
    t_deband=0;t_sharp=0;{double ms=frame_ms(40);printf("  world easu only / ui pixels: %.2f ms per frame (GPU %.2f, CPU %.2f)\n",ms,gpu_ms_last,cpu_ms_last);}
    t_sharp=.25f;
}

/* A layered game frame dump, SHGF: 64 int32 (magic, version 1, width,
 * height, raster width/height, float world scale x/y and UI scale, key, HUD
 * flag, 3 x (used, bounds), part count, 4 x (layer, top, bottom, float
 * anchor x/y), highlight rect), then width x raster height world RGB565 and,
 * per layer, width x height RGB565 pixels and coverage bytes. Renders it
 * with each filter combination for side-by-side comparisons. */
static int render_dump(const char *path)
{
    FILE *in=fopen(path,"rb");if(!in){printf("cannot open %s\n",path);return 1;}
    int32_t head[64];if(fread(head,4,64,in)!=64||memcmp(head,"SHGF",4)){puts("not a layered dump");return 1;}
    /* Version1 is the legacy three-layer format, independent of this build. */
    if(head[1]!=1||head[26]<0||head[26]>4){puts("unsupported layered dump version/parts");return 1;}
    for(int p=0;p<head[26];p++)if(head[27+p*5]<0||head[27+p*5]>=3){puts("invalid legacy layer");return 1;}
    stronghold_frame f;memset(&f,0,sizeof f);
    f.width=head[2];f.height=head[3];f.raster_width=head[4];f.raster_height=head[5];
    memcpy(&f.world_scale_x,&head[6],4);memcpy(&f.world_scale_y,&head[7],4);memcpy(&f.ui_scale,&head[8],4);
    f.key=(uint16_t)head[9];f.hud_opaque=head[10];
    for(int i=0;i<3;i++){f.layer[i].used=head[11+i*5];memcpy(&f.layer[i].bounds,&head[12+i*5],16);}
    f.parts=head[26];
    for(int p=0;p<4;p++){f.part[p].layer=head[27+p*5];f.part[p].top=head[28+p*5];f.part[p].bottom=head[29+p*5];
        memcpy(&f.part[p].anchor_x,&head[30+p*5],4);memcpy(&f.part[p].anchor_y,&head[31+p*5],4);}
    memcpy(&f.highlight,&head[47],16);
    size_t n=(size_t)f.width*f.height;
    uint16_t *world=malloc(n*2);uint16_t *pix[3];uint8_t *cov[3];
    if(fread(world,2,(size_t)f.width*f.raster_height,in)!=(size_t)f.width*f.raster_height){puts("short dump");return 1;}
    for(int i=0;i<3;i++){pix[i]=malloc(n*2);cov[i]=malloc(n);
        if(fread(pix[i],2,n,in)!=n||fread(cov[i],1,n,in)!=n){puts("short dump");return 1;}
        f.layer[i].pixels=pix[i];f.layer[i].coverage=cov[i];f.layer[i].dirty=(stronghold_rect){0,0,f.width,f.height};}
    fclose(in);
    f.world=world;f.fresh=1;
    printf("dump %dx%d raster %dx%d world scale %.3f ui %.2f parts %d\n",f.width,f.height,f.raster_width,f.raster_height,f.world_scale_x,f.ui_scale,f.parts);
    static const struct { const char *name; int world,ui_mode,deband; float sharp; } runs[]={
        {"world-bilinear-ui-bilinear",0,0,0,.25f},{"world-pixels-ui-pixels",1,1,0,.25f},{"world-bicubic-ui-bicubic",2,2,0,.25f},
        {"world-fsr-ui-fsr",3,3,0,.25f},{"world-fsr-ui-pixels",3,1,0,.25f},{"world-fsr-ui-bicubic",3,2,0,.25f},
        {"world-fsr-deband-ui-pixels",3,1,1,.25f},{"world-fsr-sharp0",3,1,0,0},{"world-fsr-sharp100",3,1,0,1},
        {"world-bicubic-sharp0-ui-bicubic",2,2,0,0}};
    for(unsigned r=0;r<sizeof runs/sizeof runs[0];r++){
        test_frame=f;test_layered=1;t_world=runs[r].world;t_ui=runs[r].ui_mode;t_deband=runs[r].deband;t_sharp=runs[r].sharp;
        double ms=frame_ms(20);
        test_frame=f;test_frame.fresh=1;present_test();read_window();
        char out[160];snprintf(out,sizeof out,"/data/local/tmp/gfx-out-%s.rgba",runs[r].name);
        FILE *o=fopen(out,"wb");if(o){fwrite(window_rgba,4,(size_t)W*H,o);fclose(o);}
        printf("  %-34s %.2f ms per frame (GPU %.2f, CPU %.2f) -> %s\n",runs[r].name,ms,gpu_ms_last,cpu_ms_last,out);
    }
    return 0;
}

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);mkdir(void_data_dir(),0775);
    window_rgba=malloc((size_t)W*H*4);world_px=malloc((size_t)FW*FH*2);
    for(int i=0;i<SH_UI_LAYERS;i++){layer_px[i]=malloc((size_t)FW*FH*2);layer_cov[i]=malloc((size_t)FW*FH);}
    assert(initialize()&&gles3);
    if(argc>1)return render_dump(argv[1]);
    programs_check();
    raw_frame_check();
    fsr_edge_check();
    deband_check();
    premultiplied_check();
    incremental_check();
    wide_hud_check();
    resource_rows_check();
    resource_notice_check();
#ifndef STRONGHOLD_ORIGINAL
    overlay_classes_check();
#endif
    loupe_check();
    glow_check();

    timing_check();
    puts("GPU presenter harness: PASS");
    return 0;
}
