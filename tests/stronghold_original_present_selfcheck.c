/* Stronghold's seven original anchor classes through the real GPU presenter.
 * Build like ddraw_present_selfcheck.c; run through adb shell. */
#define STRONGHOLD_ORIGINAL
#define main crusader_present_checks_main
#include "ddraw_present_selfcheck.c"
#undef main

static void rect(int layer, int x0, int y0, int x1, int y1, uint16_t color)
{
    uint16_t *px = (uint16_t *)test_frame.layer[layer].pixels;
    uint8_t *alpha = (uint8_t *)test_frame.layer[layer].coverage;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { px[y*FW+x] = color; alpha[y*FW+x] = 255; }
}
static void expect(int x, int y, float ax, float ay, uint16_t color)
{
    float s = test_frame.ui_scale;
    int wx, wy; window_of(ax+(x+.5f-ax)*s, ay+(y+.5f-ay)*s, &wx, &wy);
    int rgb[3]; expand(color,rgb);
    for (int i=0;i<3;i++) assert(abs(at(wx,wy)[i]-rgb[i])<=3);
}
static void scaled_letterbox(void)
{
    const int sizes[][2]={{1064,600},{1304,400}};
    for(int size=0;size<2;size++) {
        int fw=sizes[size][0],fh=sizes[size][1];
        new_frame();test_frame.width=test_frame.raster_width=fw;test_frame.height=test_frame.raster_height=fh;
        /* An opaque full-width layer grows beyond all four frame edges. */
        for(int i=0;i<fw*fh;i++){layer_px[0][i]=0xffff;layer_cov[0][i]=255;}
        use_layer(0,(stronghold_rect){0,0,fw,fh});add_part(0,0,fh,fw*.5f,fh*.5f);
        test_frame.ui_scale=1.6f;
        view v=fit_view(fw,fh,W,H);
        for(int filter=0;filter<4;filter++) {
            t_ui=filter;g.refresh=1;present_layered(&test_frame,W,H);read_window();
            for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
                if(x+.5>=v.ox&&x+.5<v.ox+fw*v.fit&&y+.5>=v.oy&&y+.5<v.oy+fh*v.fit)continue;
                const uint8_t *pixel=at(x,y);
                assert(pixel[0]==0&&pixel[1]==0&&pixel[2]==0);
            }
            for(int c=0;c<3;c++)assert(at(W/2,H/2)[c]>=252);
        }
    }
    puts("Stronghold GPU: enlarged UI preserves side/top/bottom black bars; all four filters PASS");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);mkdir(void_data_dir(),0775);
    window_rgba=malloc((size_t)W*H*4);world_px=calloc((size_t)FW*FH,2);
    for(int i=0;i<SH_UI_LAYERS;i++){layer_px[i]=malloc((size_t)FW*FH*2);layer_cov[i]=malloc((size_t)FW*FH);}
    assert(initialize()&&gles3);programs_check();raw_frame_check();
    new_frame();
    rect(0,620,90,680,115,0xffff);use_layer(0,(stronghold_rect){620,90,680,115});
    rect(1,30,FH/2+30,60,FH/2+50,0x07ff);use_layer(1,(stronghold_rect){30,FH/2+30,60,FH/2+50});
    rect(4,20,FH-90,60,FH-60,0x001f);use_layer(4,(stronghold_rect){20,FH-90,60,FH-60});
    rect(3,FW-100,10,FW-20,50,0xf800);use_layer(3,(stronghold_rect){FW-100,10,FW-20,50});
    rect(5,FW/2-30,10,FW/2+30,40,0xffe0);use_layer(5,(stronghold_rect){FW/2-30,10,FW/2+30,40});
    rect(6,FW-100,FH/2-20,FW-20,FH/2+20,0xf81f);rect(6,620,240,680,280,0xf800);
    use_layer(6,(stronghold_rect){620,240,FW-20,FH/2+20});
    rect(2,520,200,780,320,0x07e0);use_layer(2,(stronghold_rect){520,200,780,320});
    const float anchors[SH_UI_LAYERS][2]={{FW*.5f,FH},{0,0},{FW*.5f,FH*.5f},{FW,0},{0,FH},{FW*.5f,0},{FW,FH*.5f}};
    for(int order=0;order<SH_UI_LAYERS;order++){
        int i=SH_UI_ORDER[order];add_part(i,0,FH,anchors[i][0],anchors[i][1]);
    }
    assert(test_frame.parts==SH_UI_LAYERS&&test_frame.parts<=SH_UI_PARTS);
    for (int scale=0;scale<2;scale++) for(int filter=0;filter<4;filter++) {
        test_frame.ui_scale=scale?1.6f:1.f;t_world=SH_FILTER_PIXELS;t_ui=filter;g.refresh=1;
        overlay_count=0;test_frame.fresh=1;present_test();read_window();
        assert(overlay_count==SH_UI_LAYERS);
        for(int order=0;order<SH_UI_LAYERS;order++)assert(overlay_groups[order]==SH_UI_ORDER[order]);
        assert(hide_seen_groups==(1<<SH_UI_LAYERS)-1);
        expect(FW-60,30,FW,0,0xf800);expect(40,FH-75,0,FH,0x001f);
        expect(40,FH/2+40,0,0,0x07ff);expect(FW/2,25,FW*.5f,0,0xffe0);
        expect(FW-60,FH/2,FW,FH*.5f,0xf81f);
        expect(650,260,FW*.5f,FH*.5f,0x07e0);
        if(!scale)expect(650,100,FW*.5f,FH,0xffff);
    }
    puts("Stronghold GPU: seven registered anchors, top resource below old half split, left-bottom notice, dialog priority, overhang; all four filters PASS");
    scaled_letterbox();
    return 0;
}
