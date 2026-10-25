/* Keep the game's geometry, allocations and HUD consistent for custom modes. */
#include "host.h"
#include "input.h"
#include "edition.h"
#include "display.h"
#include "feedback.h"
#include <math.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#ifdef STRONGHOLD_CLASSIC
#define CAMERA_MAP 0x023443C0u
#define UI_MAIN 0x02125920u
#define UI_MODAL 0x02598E60u
#define UI_POWERS 0u
#define UI_COLOR_KEY 0x00DF53E8u
#define HUD_OFFSET 0x00F687E8u
#else
#define CAMERA_MAP 0x02DD78A0u
#define UI_MAIN 0x02BB8E00u
#define UI_MODAL 0x030D6200u
#define UI_POWERS 0x020D2B50u
#define UI_COLOR_KEY 0x00DF5568u
#define HUD_OFFSET 0x00F68C48u
#endif
#define MAP_PITCH 8112u
#define MAP_BYTES (MAP_PITCH * 2076u)
#ifdef STRONGHOLD_DISPLAY_SELFTEST
#undef CAMERA_MAP
#define CAMERA_MAP 0x10000000u
#endif

/* shown: the bounds last published; dirty: changed pixels not yet taken by the GPU presenter. */
typedef struct {
    uint32_t guest; uint16_t *pixels; uint8_t *coverage,*capture; int used, left,top,right,bottom;
    stronghold_rect shown, dirty, fill;
    int filled,fill_x,fill_y,fill_width,fill_height;
    uint16_t fill_key;
} ui_layer;
static ui_layer ui[SH_UI_LAYERS]; /* HUD, powers, center, left-bottom, center-top, right-top, right-center */
static uint16_t *world_pixels, *composed_pixels;
static size_t frame_capacity;
static uint32_t frame_map, original_pixels, original_surface;
static uint32_t hud_pixels,hud_pitch;
static int ui_open, current_group, previous_group, frame_ready, frame_active;
static int source_width, source_height, bounds_height, origin_x, origin_y;
static int raster_width,raster_height;
static int surface_width, surface_height, published_width, published_height, published_active;
static int published_source_width, published_source_height;
static int published_raster_width,published_raster_height;
static int published_used[SH_UI_LAYERS],published_hud_opaque,gpu_taken;
static int resource_capture,resource_rows=128,published_resource_rows=128;
static int overlay_active,resource_fit_rows=128,resource_fit_right;
static int animation_capture;
static int overlay_previous_group=-1;
static int top_overlay_used,top_overlay_right,hud_top;
static float published_anchor[SH_UI_LAYERS][2];
static float ui_scale=1, world_zoom=1, published_ui_scale=1, published_present_scale=1;
static float powers_scale, published_powers_scale, cutout_left;
static float layer_scale(int group)
{ return group==1&&UI_POWERS&&published_powers_scale>0?published_powers_scale:published_ui_scale; }
static float published_world_zoom=1;
static float requested_render_scale=1, render_scale=1;
static double unsnapped_zoom=1;
/* The 1:1 detent engages once per touch gesture: 0 armed, 1 holding, 2 spent. */
#define SNAP_LOW .94
#define SNAP_HIGH 1.06
static int snap_state;
static int highlight_left,highlight_top,highlight_width,highlight_height;
static int filter_mode=2;
static float filter_sharpness=.25f;
int stronghold_display_capture_active;
static unsigned raster_clear_depth;
int stronghold_display_clear_batch_active;
static struct {
    uint32_t context, pixels, pitch;
    int capture, group, valid, left,top,right,bottom;
} clear_batch;
static double camera_fraction_x, camera_fraction_y, pending_x, pending_y;
static int32_t camera_last_x, camera_last_y;
static int camera_known, camera_last_native;
static pthread_mutex_t display_lock=PTHREAD_MUTEX_INITIALIZER;
static int smooth_camera=1,requested_smooth_camera=1;
static int requested_width, requested_height;

void stronghold_display_powers_scale(float scale)
{
    if (!isfinite(scale)) return;
    pthread_mutex_lock(&display_lock); powers_scale=fmaxf(.75f,fminf(3,scale)); pthread_mutex_unlock(&display_lock);
}
void stronghold_display_cutout(float left)
{
    if (!isfinite(left)||left<0||left>.5f) return;
    pthread_mutex_lock(&display_lock);cutout_left=left;pthread_mutex_unlock(&display_lock);
}
void stronghold_display_bar(uint32_t stack)
{
    if (!void_vm_writable(stack+4,4)) return;
    pthread_mutex_lock(&display_lock);
    MEM32(stack+4)+=(uint32_t)stronghold_notice_margin(requested_width,requested_height,
        surface_width,surface_height,cutout_left,fminf(.75f,ui_scale));
    pthread_mutex_unlock(&display_lock);
}

void stronghold_display_smooth_camera(int enabled)
{
    pthread_mutex_lock(&display_lock);
    requested_smooth_camera=!!enabled;
    pthread_mutex_unlock(&display_lock);
}

/* A terrain diamond spans 32 by 16 world pixels, independent of view zoom. */
static int32_t camera_coordinate(double value,int step)
{ return (int32_t)(llround(value/(smooth_camera?1:step))*(smooth_camera?1:step)); }
static uint16_t color_key,published_key;
uint32_t void_ddraw_redirect_pixels(uint32_t surface,uint32_t pixels);

static void layer_anchor(int group,int width,int height,double *ax,double *ay)
{
    *ax=group==3?0:(group==1||group==5||group==6)?width:width*.5;
    *ay=group==0||group==3||group==7?height:group==1?56:group==4||group==5?0:height*.5;
}

static int valid_size(int width, int height)
{
    return width >= 800 && width <= 4056 && height >= 600 && height <= 2076 &&
           (int64_t)width * 3 >= (int64_t)height * 4 && width <= height * 3;
}

void stronghold_display_configure(int width, int height)
{
    requested_width = requested_height = 0;
    if (!width && !height) return;
    if (!valid_size(width, height)) {
        void_log("[display] rejected custom mode %dx%d", width, height);
        return;
    }
    /* Original software sprite paths assume complete groups of eight pixels. */
    requested_width = (width + 7) & ~7;
    requested_height = height;
    void_input_set_screen_size(requested_width, requested_height);
    void_log("[display] requested %dx%d", requested_width, requested_height);
}

int stronghold_display_requested(int *width, int *height)
{
    if (!requested_width) return 0;
    *width = requested_width; *height = requested_height;
    return 1;
}

/* Inserted immediately before the original mode calculator derives pitch,
 * buffer sizes, centering and map/HUD geometry, including its direct callers. */
int stronghold_display_dimensions(uint32_t screen)
{
    if (!requested_width || screen != GAME_SCREEN || !void_vm_writable(screen, 0x60)) return 0;
    MEM32(screen + 0x18) = (uint32_t)requested_width;
    MEM32(screen + 0x1c) = (uint32_t)requested_height;
    MEM32(screen + 0x5c) = 3;
    return 1;
}

uint32_t stronghold_display_width(uint32_t original)
{ return requested_width ? (uint32_t)requested_width : original; }

uint32_t stronghold_display_height(uint32_t original)
{ return requested_height ? (uint32_t)requested_height : original; }

uint32_t stronghold_display_half_width(void)
{ return requested_width ? (uint32_t)(requested_width - 800) / 2 : 240; }

/* The original mode table has these exact counts for all 16 presets, both
 * zoom levels and both HUD heights. Camera limits consume them immediately. */
void stronghold_display_viewport(uint32_t map)
{
    if (!requested_width || !void_vm_writable(map + 0x88, 12) ||
        !void_vm_writable(map + 0x18b730, 8)) return;
    uint32_t width = MEM32(map + 0x18b730), height = MEM32(map + 0x18b734);
    if (frame_active && map == frame_map) {
        width=(uint32_t)source_width; height=(uint32_t)source_height;
        MEM32(map+0x18b730)=width; MEM32(map+0x18b734)=height;
    } else if (width != (uint32_t)requested_width ||
        (height != (uint32_t)requested_height && height != (uint32_t)requested_height - 128)) return;
    uint32_t scale = MEM32(map + 0x90) ? 2 : 1;
    MEM32(map + 0x88) = ((width + 31) / 32 + 5) * scale;
    MEM32(map + 0x8c) = ((frame_active&&map==frame_map?(uint32_t)bounds_height:height) / 8 + 4) * scale;
}

void stronghold_display_clear(uint32_t stack)
{
    if (!requested_width || !void_vm_writable(stack, 20)) return;
    MEM32(stack + 8) = (uint32_t)requested_width - 1;
    MEM32(stack + 12) = (uint32_t)requested_height - 1;
}

/* Larger stock TGX edges contain the same inner border. Cropping their outer
 * excess fills arbitrary widths around the game's centered 800-pixel panel. */
uint32_t stronghold_display_edge_name(uint32_t name)
{
    if (!requested_width || requested_width == 1280) return name;
    static const uint32_t names[][2] = {
        {0x005a3374u, 0x005a3334u}, {0x005a3404u, 0x005a33c4u},
        {0x005a358cu, 0x005a34ccu}, {0x005a3574u, 0x005a34b4u}
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (name == names[i][0] + GAME_RESOURCE_DELTA) return names[i][1] + GAME_RESOURCE_DELTA;
    return name;
}

/* Stack is the three already-pushed original arguments: image, x, y.
 * A wider edge needs the game's existing clipped TGX path (same ABI). */
int stronghold_display_hud_edge(uint32_t renderer, uint32_t stack, int right)
{
    if (!requested_width || !void_vm_writable(stack, 12)) return 0;
    uint32_t index = MEM32(stack);
    if (index >= 100 || !void_vm_writable(renderer + 0x16c20c + index * 16, 8)) return 0;
    uint32_t width = MEM32(renderer + 0x16c20c + index * 16);
    uint32_t height = MEM32(renderer + 0x16c210 + index * 16);
    if (width > 1024 || height > 256) return 0;
    int x = right ? (int32_t)MEM32(stack + 4) + (requested_width - 1280) / 2
                  : (requested_width - 800) / 2 - (int)width;
    MEM32(stack + 4) = (uint32_t)x;
    MEM32(stack + 8) = (uint32_t)(requested_height - (int)height);
    return requested_width != 1280;
}

void stronghold_display_hud_clip(uint32_t renderer, uint32_t saved[4], int restore)
{
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t address = renderer + 0x16c854 + i * 4;
        if (restore) MEM32(address) = saved[i];
        else {
            saved[i] = MEM32(address);
            MEM32(address) = i == 2 ? (uint32_t)requested_width : i == 3 ? (uint32_t)requested_height : 0;
        }
    }
}

void stronghold_display_surface_size(int width,int height)
{
    if(width<=0||height<=0)return;
    pthread_mutex_lock(&display_lock);
    surface_width=width;surface_height=height;
    pthread_mutex_unlock(&display_lock);
}

/* This inverse is shared by pan, pinch, UI hits and world aiming. */
static int surface_point(float nx,float ny,double *x,double *y,double *fit)
{
    int w=published_width?published_width:requested_width;
    int h=published_height?published_height:requested_height;
    int sw=surface_width?surface_width:w,sh=surface_height?surface_height:h;
    if(!isfinite(nx)||!isfinite(ny)||w<=0||h<=0||sw<=0||sh<=0)return 0;
    double s=fmin((double)sw/w,(double)sh/h);
    *x=(nx*sw-(sw-w*s)*.5)/s;*y=(ny*sh-(sh-h*s)*.5)/s;
    if(fit)*fit=s;
    return *x>=0&&*y>=0&&*x<w&&*y<h;
}

void stronghold_display_set_ui_scale(float scale)
{
    if(!isfinite(scale))return;
    pthread_mutex_lock(&display_lock);
    ui_scale=fmaxf(.75f,fminf(2.f,scale));
    pthread_mutex_unlock(&display_lock);
}

void stronghold_display_set_render_scale(float scale)
{
    if(!isfinite(scale))return;
    pthread_mutex_lock(&display_lock);
    requested_render_scale=fmaxf(.5f,fminf(1.f,scale));
    pthread_mutex_unlock(&display_lock);
}

float stronghold_display_world_zoom(void)
{
    pthread_mutex_lock(&display_lock);
    float zoom=published_world_zoom;
    pthread_mutex_unlock(&display_lock);
    return zoom;
}

void stronghold_display_transform(float nx,float ny,float ndx,float ndy,float factor)
{
    if(!isfinite(ndx)||!isfinite(ndy)||!isfinite(factor)||factor<=0)return;
    pthread_mutex_lock(&display_lock);
    double x,y,fit;
    if(!published_active||!surface_point(nx,ny,&x,&y,&fit)){
        pthread_mutex_unlock(&display_lock);return;
    }
    double old=world_zoom,next=old;
    /* A dead band makes the original pixel grid reachable by a pinch, once
     * per gesture. Leaving it continues from 1:1 instead of jumping by the
     * band's width. Panning with factor1 must not change the zoom. */
    if(factor!=1.f){
        unsnapped_zoom=fmax(.5,fmin(2.5,unsnapped_zoom*factor));
        next=unsnapped_zoom;
        int inside=unsnapped_zoom>=SNAP_LOW&&unsnapped_zoom<=SNAP_HIGH;
        /* Arriving at 1:1 from another zoom is felt as a tick. */
        if(!snap_state&&inside){snap_state=1;if(old!=1)stronghold_feedback(SH_FEEDBACK_ZOOM_SNAP);}
        if(snap_state==1){
            if(inside)next=1;
            else{
                unsnapped_zoom/=unsnapped_zoom>1?SNAP_HIGH:SNAP_LOW;
                next=unsnapped_zoom;snap_state=2;
            }
        }
    }
    int sw=surface_width?surface_width:published_width;
    int sh=surface_height?surface_height:published_height;
    /* Main pick4E9180 adds screenY once (twice in native half-size zoom).
     * The doubled Y in terrain ray4E6C20 is its height search, not a camera unit. */
    pending_x-=ndx*sw/fit/old;pending_y-=ndy*sh/fit/old;
    pending_x+=x*(1/old-1/next);pending_y+=y*(1/old-1/next);
    world_zoom=(float)next;
    pthread_mutex_unlock(&display_lock);
}

void stronghold_display_zoom_session(void)
{
    pthread_mutex_lock(&display_lock);
    snap_state=0;
    if(world_zoom==1.f)unsnapped_zoom=1;
    pthread_mutex_unlock(&display_lock);
}

void stronghold_display_zoom_reset(void)
{
    pthread_mutex_lock(&display_lock);
    int w=published_width?published_width:requested_width;
    int h=published_height?published_height:requested_height;
    if(published_active&&world_zoom!=1.f&&w>0&&h>0){
        /* The centre of the view stays fixed, as a pinch keeps its centroid. */
        double old=world_zoom;
        pending_x+=w*.5*(1/old-1);pending_y+=h*.5*(1/old-1);
        world_zoom=1;
    }
    unsnapped_zoom=1;snap_state=2;
    pthread_mutex_unlock(&display_lock);
}

void stronghold_display_highlight(int left,int top,int width,int height)
{
    pthread_mutex_lock(&display_lock);
    highlight_left=left;highlight_top=top;
    highlight_width=width>0&&height>0?width:0;highlight_height=width>0&&height>0?height:0;
    pthread_mutex_unlock(&display_lock);
}

/* Per-layer filters and the placement loupe: recorded here, drawn by the
 * presenter (ddraw_present.c). FSR won side-by-side comparisons on game
 * frames for both: crisp edges without the halos of bicubic+CAS or the
 * stair-steps sharp pixels leave at non-integer scales (HUD, icons, digits). */
#define DEFAULT_WORLD_FILTER SH_FILTER_FSR
#define DEFAULT_UI_FILTER SH_FILTER_FSR
static int world_filter=DEFAULT_WORLD_FILTER,ui_filter=DEFAULT_UI_FILTER,deband_enabled,crisp_text_enabled=1;
static int magnifier_active;
static float magnifier_x,magnifier_y;

/* Legacy single filter (0 bilinear, 1 sharp pixels, 2 bicubic): both the
 * world and the UI use it until stronghold_display_set_filters() is called. */
void stronghold_display_set_image_filter(int mode,float sharpness)
{
    pthread_mutex_lock(&display_lock);
    filter_mode=mode>=0&&mode<=2?mode:2;
    world_filter=ui_filter=filter_mode;
    filter_sharpness=isfinite(sharpness)?fmaxf(0,fminf(1,sharpness)):0;
    pthread_mutex_unlock(&display_lock);
}

void stronghold_display_set_filters(int world,int ui_mode,float sharpness,int deband,int crisp_text)
{
    pthread_mutex_lock(&display_lock);
    world_filter=world>=SH_FILTER_BILINEAR&&world<=SH_FILTER_FSR?world:DEFAULT_WORLD_FILTER;
    ui_filter=ui_mode>=SH_FILTER_BILINEAR&&ui_mode<=SH_FILTER_FSR?ui_mode:DEFAULT_UI_FILTER;
    /* The OpenGL ES 2 fallback has one filter; FSR needs ES 3, bicubic is closest. */
    filter_mode=world_filter==SH_FILTER_FSR?2:world_filter;
    filter_sharpness=isfinite(sharpness)?fmaxf(0,fminf(1,sharpness)):0;
    deband_enabled=deband!=0;crisp_text_enabled=crisp_text!=0;
    pthread_mutex_unlock(&display_lock);
}

void stronghold_display_filters(int *world,int *ui_mode,float *sharpness,int *deband,int *crisp_text)
{
    pthread_mutex_lock(&display_lock);
    if(world)*world=world_filter;
    if(ui_mode)*ui_mode=ui_filter;
    if(sharpness)*sharpness=filter_sharpness;
    if(deband)*deband=deband_enabled;
    if(crisp_text)*crisp_text=crisp_text_enabled;
    pthread_mutex_unlock(&display_lock);
}

void stronghold_display_magnifier(int active,float nx,float ny)
{
    pthread_mutex_lock(&display_lock);
    magnifier_active=active&&isfinite(nx)&&isfinite(ny);
    if(magnifier_active){magnifier_x=fmaxf(0,fminf(1,nx));magnifier_y=fmaxf(0,fminf(1,ny));}
    pthread_mutex_unlock(&display_lock);
}

int stronghold_display_loupe(float *nx,float *ny)
{
    pthread_mutex_lock(&display_lock);
    int active=magnifier_active;
    if(active){*nx=magnifier_x;*ny=magnifier_y;}
    pthread_mutex_unlock(&display_lock);
    return active;
}

int stronghold_display_locate(uint32_t address,int *x,int *y)
{
    for(int i=0;i<SH_UI_LAYERS;i++)if(ui[i].guest&&address>=ui[i].guest&&address<ui[i].guest+MAP_BYTES){
        uint32_t offset=address-ui[i].guest;
        *x=(int)(offset%MAP_PITCH/2)-origin_x;*y=(int)(offset/MAP_PITCH)-origin_y;
        return i;
    }
    /* The HUD surface is also the whole frame of menus (original present
     * 4705E0/4707F0), before any match has opened a UI pass: read it live. */
    uint32_t hud=MEM32(GAME_SCREEN+0xd4),pitch=MEM32(GAME_SCREEN+0x40);
    int height=requested_height>0?requested_height:(int)MEM32(GAME_SCREEN+0x1c);
    if(hud&&pitch>=2&&height>0&&address>=hud&&(uint64_t)address<(uint64_t)hud+(uint64_t)pitch*(uint32_t)height){
        uint32_t offset=address-hud;
        *x=(int)(offset%pitch/2);*y=(int)(offset/pitch);
        return SH_UI_LAYERS;
    }
    return -1;
}

int stronghold_display_layer_view(int group,float lx,float ly,float *vx,float *vy,float *scale)
{
    pthread_mutex_lock(&display_lock);
    int w=published_width?published_width:requested_width,h=published_height?published_height:requested_height;
    int sw=surface_width?surface_width:w,sh=surface_height?surface_height:h,ok=w>0&&h>0&&sw>0&&sh>0;
    if(ok){
        /* The same anchors and order as the composition: scale about the anchor, then fit. */
        double fit=fmin((double)sw/w,(double)sh/h),s=1,gx=lx,gy=ly;
        if(group>=0&&group<SH_UI_LAYERS&&published_active){
            if(group==1&&!UI_POWERS)group=2;
            double ax=published_anchor[group][0],ay=published_anchor[group][1];
            if(group==0&&ly<published_resource_rows)ax=ay=0;
            s=layer_scale(group);gx=ax+(lx-ax)*s;gy=ay+(ly-ay)*s;
        }
        *vx=(float)((sw-w*fit)*.5+gx*fit);*vy=(float)((sh-h*fit)*.5+gy*fit);*scale=(float)(s*fit);
    }
    pthread_mutex_unlock(&display_lock);
    return ok;
}

/* Crisp text (text_overlay.c; prototypes in text_overlay.h): the setting,
 * the open pass's coverage so far, and for the old presenter the CPU layers
 * of a newly published frame before its first composition. */
int stronghold_display_crisp_text(void)
{
    pthread_mutex_lock(&display_lock);
    int on=crisp_text_enabled;
    pthread_mutex_unlock(&display_lock);
    return on;
}

const uint8_t *stronghold_display_capture_row(int *group,int y,int *width)
{
    *group=-1;
    if(!ui_open||!stronghold_display_capture_active||y<0||y>=requested_height||!ui[current_group].capture)return NULL;
    *group=current_group;*width=requested_width;
    return ui[current_group].capture+(size_t)y*requested_width;
}

int stronghold_display_text_layers(uint16_t *layers[SH_UI_LAYERS],uint8_t *coverage[SH_UI_LAYERS],int bounds[SH_UI_LAYERS][4],int *width,int *height)
{
    if(ui_open||frame_ready!=1||!requested_width)return 0;
    for(int i=0;i<SH_UI_LAYERS;i++){
        layers[i]=published_used[i]?ui[i].pixels:NULL;coverage[i]=published_used[i]?ui[i].coverage:NULL;
        bounds[i][0]=ui[i].left;bounds[i][1]=ui[i].top;bounds[i][2]=ui[i].right;bounds[i][3]=ui[i].bottom;
    }
    *width=requested_width;*height=requested_height;
    return 1;
}

int stronghold_display_image_filter(float *sharpness)
{
    pthread_mutex_lock(&display_lock);
    int mode=filter_mode;*sharpness=filter_sharpness;
    pthread_mutex_unlock(&display_lock);
    return mode;
}

void stronghold_display_frame_begin(void)
{
    stronghold_perf_frame_begin();
    /* An aborted UI pass must never leave a borrowed framebuffer installed. */
    if(ui_open){
        MEM32(GAME_SCREEN+0xd8)=original_pixels;
        void_ddraw_redirect_pixels(original_surface,original_pixels);
    }
    stronghold_display_capture_active=0;
    raster_clear_depth=0;
    stronghold_display_clear_batch_active=0;
    ui_open=frame_ready=frame_active=0;
    resource_capture=overlay_active=0;resource_rows=resource_fit_rows=128;resource_fit_right=0;overlay_previous_group=-1;
    top_overlay_used=top_overlay_right=0;hud_top=requested_height-128;
    for(int i=0;i<SH_UI_LAYERS;i++)ui[i].used=0;
}

void stronghold_display_world_begin(uint32_t map)
{
    if(!requested_width||map!=CAMERA_MAP||!void_vm_writable(map,0x18b738))return;
    frame_map=map;frame_active=1;
    pthread_mutex_lock(&display_lock);
    render_scale=requested_render_scale;
    smooth_camera=requested_smooth_camera;
    int32_t x=(int32_t)MEM32(map+0x78),y=(int32_t)MEM32(map+0x7c);
    int native=MEM32(map+0x90)!=0;
    if(!camera_known&&native)world_zoom=(float)(unsnapped_zoom=.5);
    if(!camera_known||x!=camera_last_x||y!=camera_last_y){
        camera_fraction_x=x;camera_fraction_y=y;
    }
    if(camera_known&&native!=camera_last_native&&!pending_x&&!pending_y)
        world_zoom=(float)(unsnapped_zoom=native?.5:1);
    int next_native=world_zoom<1;
    camera_fraction_x+=pending_x+(native?160:0)-(next_native?160:0);
    camera_fraction_y+=pending_y;
    pending_x=pending_y=0;
    MEM32(map+0x90)=(uint32_t)next_native;
    MEM32(map+0x78)=(uint32_t)camera_coordinate(camera_fraction_x,32);
    MEM32(map+0x7c)=(uint32_t)camera_coordinate(camera_fraction_y,16);
    double residual=world_zoom/(next_native?.5:1);
    source_width=(int)ceil(requested_width/residual);
    source_height=(int)ceil(requested_height/residual);
    bounds_height=(int)ceil((requested_height-128*ui_scale)/residual);
    camera_last_native=next_native;camera_known=1;
    pthread_mutex_unlock(&display_lock);
}

void stronghold_display_world_render(uint32_t map)
{
    if(frame_active&&map==frame_map)
        MEM32(map+0x8c)=((uint32_t)source_height/8+4)*(MEM32(map+0x90)?2:1);
}

uint32_t stronghold_display_world_width(void)
{return (uint32_t)(source_width?source_width:requested_width);}
uint32_t stronghold_display_world_height(void)
{return (uint32_t)(source_height?source_height:requested_height);}

static int allocate_frames(void)
{
    static int layout_width,layout_height;
    size_t count=(size_t)requested_width*requested_height;
    if(count<=frame_capacity){
        if(layout_width==requested_width&&layout_height==requested_height)return 1;
        /* Same buffers, another row pitch: restart the published layers. */
        pthread_mutex_lock(&display_lock);
        for(int i=0;i<SH_UI_LAYERS;i++){
            if(ui[i].coverage)memset(ui[i].coverage,0,frame_capacity);
            ui[i].shown=(stronghold_rect){0,0,0,0};
            ui[i].dirty=(stronghold_rect){0,0,requested_width,requested_height};
        }
        layout_width=requested_width;layout_height=requested_height;
        pthread_mutex_unlock(&display_lock);return 1;
    }
    uint16_t *world=malloc(count*2),*composed=malloc(count*2);
    if(!world||!composed){
        free(world);free(composed);void_log("[display] cannot allocate world buffers");return 0;
    }
    pthread_mutex_lock(&display_lock);
    free(world_pixels);free(composed_pixels);world_pixels=world;composed_pixels=composed;
    for(int i=0;i<SH_UI_LAYERS;i++){
        free(ui[i].pixels);free(ui[i].coverage);free(ui[i].capture);
        ui[i].pixels=NULL;ui[i].coverage=ui[i].capture=NULL;
        ui[i].filled=0;
        ui[i].shown=ui[i].dirty=(stronghold_rect){0,0,0,0};
    }
    frame_capacity=count;layout_width=requested_width;layout_height=requested_height;
    pthread_mutex_unlock(&display_lock);return 1;
}

/* Only original overlay classes actually drawn need their frame-sized copy. */
static int allocate_layer(int group)
{
    if(ui[group].pixels)return 1;
    uint16_t *pixels=malloc(frame_capacity*2);
    uint8_t *coverage=calloc(frame_capacity,1),*capture=malloc(frame_capacity);
    if(!pixels||!coverage||!capture){free(pixels);free(coverage);free(capture);return 0;}
    ui[group].pixels=pixels;ui[group].coverage=coverage;ui[group].capture=capture;
    ui[group].dirty=(stronghold_rect){0,0,requested_width,requested_height};
    return 1;
}

static void rect_union(stronghold_rect *r,int left,int top,int right,int bottom);
static int select_group(int group)
{
    if(!allocate_layer(group))return 0;
    if(!ui[group].guest)ui[group].guest=void_vm_alloc(0,MAP_BYTES,0x3000,4);
    if(!ui[group].guest)return 0;
    if(!ui[group].used){
        int width=requested_width,height=requested_height;
        uint16_t key=color_key;
        ui_layer *l=&ui[group];
        stronghold_rect r=l->fill;
        rect_union(&r,l->left,l->top,l->right,l->bottom);
        if(!l->filled||l->fill_x!=origin_x||l->fill_y!=origin_y||
           l->fill_width!=width||l->fill_height!=height||l->fill_key!=key)
            r=(stronghold_rect){0,0,width,height};
        for(int y=r.top;y<r.bottom;y++){
            uint16_t *row=GPTR(ui[group].guest+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)origin_x);
            for(int x=r.left;x<r.right;x++)row[x]=key;
            memset(l->capture+(size_t)y*width+r.left,0,(size_t)(r.right-r.left));
        }
        l->filled=1;l->fill_x=origin_x;l->fill_y=origin_y;l->fill_width=width;l->fill_height=height;l->fill_key=key;
        l->fill=(stronghold_rect){0,0,0,0};
        l->left=width;l->top=height;l->right=l->bottom=0;
        ui[group].used=1;
    }
    if(!void_ddraw_redirect_pixels(original_surface,ui[group].guest))return 0;
    MEM32(GAME_SCREEN+0xd8)=ui[group].guest;
    current_group=group;return 1;
}

static void capture_world(void)
{
    raster_width=(int)ceil(source_width*render_scale);
    raster_height=(int)ceil(source_height*render_scale);
    if(raster_width<1)raster_width=1;if(raster_height<1)raster_height=1;
    int columns[4056];
    if(raster_width!=source_width)
        for(int x=0;x<raster_width;x++)columns[x]=(int)((int64_t)x*source_width/raster_width);
    for(int y=0;y<raster_height;y++){
        int sy=raster_height==source_height?y:(int)((int64_t)y*source_height/raster_height);
        const uint16_t *src=GPTR(original_pixels+MAP_PITCH*(uint32_t)(origin_y+sy)+2u*(uint32_t)origin_x);
        uint16_t *dst=world_pixels+(size_t)y*requested_width;
        if(raster_width==source_width)memcpy(dst,src,(size_t)source_width*2);
        else for(int x=0;x<raster_width;x++)dst[x]=src[columns[x]];
    }
}

void stronghold_display_ui_begin(uint32_t map)
{
    if(!frame_active||map!=frame_map||!allocate_frames())return;
    original_pixels=MEM32(GAME_SCREEN+0xd8);original_surface=MEM32(GAME_SCREEN+0x100);
    hud_pixels=MEM32(GAME_SCREEN+0xd4);hud_pitch=MEM32(GAME_SCREEN+0x40);
    origin_x=(int32_t)MEM32(map+0x80);origin_y=(int32_t)MEM32(map+0x84);
    if(origin_x<0||origin_y<0||origin_x+requested_width>4056||origin_y+requested_height>2076||
       !void_vm_readable(original_pixels,MAP_BYTES))return;
    color_key=MEM16(UI_COLOR_KEY);
    camera_last_x=(int32_t)MEM32(map+0x78);camera_last_y=(int32_t)MEM32(map+0x7c);
    if(camera_last_x!=camera_coordinate(camera_fraction_x,32))camera_fraction_x=camera_last_x;
    if(camera_last_y!=camera_coordinate(camera_fraction_y,16))camera_fraction_y=camera_last_y;
    capture_world();
    if(select_group(0))stronghold_display_capture_active=ui_open=1;
}

/* Called only for original raster stores. In mode0 the EXE writes its
 * persistent UI surface, not screen+D8: mirror those writes while their
 * owning root is still known. This includes cached dialog backgrounds/fonts. */
void stronghold_display_blended(uint32_t address,unsigned bytes,unsigned background)
{
    if(!ui_open||!bytes||(bytes&1u)||bytes>MAP_PITCH)return;
    uint32_t target=ui[current_group].guest;
    uint32_t hud=hud_pixels,hp=hud_pitch;
    int x,y,d4=0;uint32_t row_byte,pitch;
    if(address>=target&&(uint64_t)address+bytes<=(uint64_t)target+MAP_BYTES){
        uint32_t offset=address-target;
        row_byte=offset%MAP_PITCH;pitch=MAP_PITCH;
        x=(int)(row_byte/2)-origin_x;y=(int)(offset/MAP_PITCH)-origin_y;
    }else if(hp&&address>=hud&&(uint64_t)address+bytes<=(uint64_t)hud+hp*(uint32_t)requested_height){
        uint32_t offset=address-hud;row_byte=offset%hp;pitch=hp;
        x=(int)(row_byte/2);y=(int)(offset/hp);d4=1;
        /* The persistent UI already contains its complete original background. */
        background=0;
    }else return;
    /* Callers publish whole RGB565 row spans (including ETO_OPAQUE fills).
     * Accept a bounded span, clip its visible intersection, never wrap it
     * into the next physical row or infer transparency from its RGB. */
    if((row_byte&1u)||bytes>pitch-row_byte||y<0||y>=requested_height)return;
    int left=x<0?0:x,right=x+(int)(bytes/2);
    if(right>requested_width)right=requested_width;
    if(left>=right)return;
    address+=2u*(uint32_t)(left-x);bytes=2u*(uint32_t)(right-left);x=left;
    if(d4)memcpy(GPTR(target+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)(origin_x+x)),GPTR(address),bytes);
    uint8_t *a=ui[current_group].capture+(size_t)y*requested_width+x;
    /* These writes belong to explicit original HUD-clear call sites. This
     * is operation provenance, not a test of the RGB value being painted. */
    if(raster_clear_depth){memset(a,0,bytes/2);return;}
    if(background>255)background=255;
    if(resource_capture&&current_group==0){
        top_overlay_used=1;
        if(right>top_overlay_right)top_overlay_right=right;
        if(y+4>resource_rows)resource_rows=y+4<requested_height?y+4:requested_height;
        if(resource_capture==1){
            if(right>resource_fit_right)resource_fit_right=right;
            if(y+4>resource_fit_rows)resource_fit_rows=y+4<requested_height?y+4:requested_height;
        }
    }else if(!overlay_active&&current_group==0&&y>=128&&y<hud_top)hud_top=y;
    if(!background)memset(a,255,bytes/2);
    else for(unsigned i=0;i<bytes/2;i++)a[i]=(uint8_t)(255-((255-a[i])*background+127)/255);
    if(x<ui[current_group].left)ui[current_group].left=x;
    if(x+(int)(bytes/2)>ui[current_group].right)ui[current_group].right=x+(int)(bytes/2);
    if(y<ui[current_group].top)ui[current_group].top=y;
    if(y>=ui[current_group].bottom)ui[current_group].bottom=y+1;
}
void stronghold_display_written(uint32_t address,unsigned bytes)
{stronghold_display_blended(address,bytes,0);}
/* Preserve the original overlay anchors (4AFE80/4B0010 flags +1C).
 * Resource rows can extend below128; left+bottom messages must not inherit
 * the centered construction panel's anchor and lose their first words. */
void stronghold_display_resource_overlay(uint32_t item,int before)
{
#ifdef STRONGHOLD_CLASSIC
    const uint32_t draw=0x00433F40u;
#else
    const uint32_t draw=0x00434150u;
#endif
    resource_capture=0;
    overlay_active=before;
    if(!before){
        if(overlay_previous_group>=0){select_group(overlay_previous_group);overlay_previous_group=-1;}
        return;
    }
    overlay_previous_group=-1;
    if(!ui_open||!void_vm_readable(item+0x18,8))return;
    unsigned flags=MEM32(item+0x1c)&15u;
    int group=flags==0?2:flags==1?4:flags==6?3:flags==8?6:flags==9?5:0;
    int timer=MEM32(item+8)==26||MEM32(item+8)==27;
    if(timer)group=0;
    /* Player hover art must survive later clears/copies of the persistent HUD. */
    if(MEM32(item+8)==21)group=7;
    if(MEM32(item+0x18)==draw)group=0;
    if(group!=current_group){
        int previous=current_group;
        if(select_group(group))overlay_previous_group=previous;
    }
    resource_capture=current_group==0?(MEM32(item+0x18)==draw?1:flags==5||timer?2:0):0;
}

void stronghold_display_resource_animation(int before)
{
    if(before){animation_capture=resource_capture;if(resource_capture==1)resource_capture=2;}
    else resource_capture=animation_capture;
}
/* Scoped original HUD clears still execute the original clipper and all its
 * pixel stores. Capture is suspended inside that loop, then its performed
 * rectangle is mirrored and erased from coverage in one pass per row. */
void stronghold_display_clear_begin(uint32_t context)
{
    if(raster_clear_depth++)return;
    clear_batch.valid=0;
    clear_batch.context=context;
    clear_batch.capture=stronghold_display_capture_active;
    clear_batch.group=current_group;
    if(!ui_open||!clear_batch.capture||!void_vm_readable(context,0x44))return;
    stronghold_display_clear_batch_active=1;
    stronghold_display_capture_active=0;
}

void stronghold_display_clear_clipped(uint32_t result)
{
    if(!stronghold_display_clear_batch_active||raster_clear_depth!=1)return;
    clear_batch.valid=0;
    if(!result)return;
    uint32_t context=clear_batch.context;
    clear_batch.pixels=MEM32(context+4);clear_batch.pitch=MEM32(context+8);
    clear_batch.left=(int32_t)MEM32(context+0x30);clear_batch.top=(int32_t)MEM32(context+0x34);
    clear_batch.right=(int32_t)MEM32(context+0x38);clear_batch.bottom=(int32_t)MEM32(context+0x3c);
    clear_batch.valid=clear_batch.left>=0&&clear_batch.top>=0&&
                      clear_batch.right>=clear_batch.left&&clear_batch.bottom>=clear_batch.top;
}

static void publish_clear_rectangle(void)
{
    int group=clear_batch.group;
    uint32_t source=clear_batch.pixels,pitch=clear_batch.pitch,target=ui[group].guest;
    int left=clear_batch.left,top=clear_batch.top,right=clear_batch.right,bottom=clear_batch.bottom;
    int d4=source==hud_pixels&&pitch==hud_pitch;
    if(!d4){
        if(source!=target||pitch!=MAP_PITCH)return;
        left-=origin_x;right-=origin_x;top-=origin_y;bottom-=origin_y;
    }
    if(left<0)left=0;if(top<0)top=0;
    if(right>=requested_width)right=requested_width-1;
    if(bottom>=requested_height)bottom=requested_height-1;
    if(left>right||top>bottom)return;
    size_t width=(size_t)(right-left+1);
    for(int y=top;y<=bottom;y++){
        if(d4){
            uint64_t from=(uint64_t)source+(uint64_t)pitch*(uint32_t)y+2u*(uint32_t)left;
            uint32_t to=target+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)(origin_x+left);
            if(from>UINT32_MAX||!void_vm_readable((uint32_t)from,(uint32_t)width*2))continue;
            memcpy(GPTR(to),GPTR((uint32_t)from),width*2);
        }
        memset(ui[group].capture+(size_t)y*requested_width+left,0,width);
    }
}

void stronghold_display_clear_scope(int enter)
{
    /* The per-store scope remains useful when a direct renderer operation
     * lacks the proven fill/clipper seam; its RGB never determines opacity. */
    if(enter){raster_clear_depth++;return;}
    if(!raster_clear_depth)return;
    if(--raster_clear_depth||!stronghold_display_clear_batch_active)return;
    if(clear_batch.valid)publish_clear_rectangle();
    stronghold_display_clear_batch_active=0;
    stronghold_display_capture_active=clear_batch.capture;
}

void stronghold_display_ui_group(uint32_t root,int enter)
{
    if(!ui_open)return;
    if(!enter){select_group(previous_group);return;}
    previous_group=current_group;
    if(!void_vm_readable(root+0x2c,4)||!MEM32(root+0x2c)||MEM32(root+0x2c)==UINT32_MAX)return;
    int group=MEM32(root+0x2c)==19?0:(UI_POWERS&&root==UI_POWERS)?1:2;
    if(!select_group(group))void_log("[display] UI layer allocation/redirect failed");
}

static void copy_hud_rect(uint32_t hud,uint32_t pitch,int left,int top,int right,int bottom)
{
    if(left<0)left=0;if(top<0)top=0;
    if(right>requested_width)right=requested_width;if(bottom>requested_height)bottom=requested_height;
    if(left>=right||top>=bottom)return;
    for(int y=top;y<bottom;y++){
        memcpy(GPTR(ui[0].guest+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)(origin_x+left)),
               GPTR(hud+pitch*(uint32_t)y+2u*(uint32_t)left),(size_t)(right-left)*2);
        memset(ui[0].capture+(size_t)y*requested_width+left,255,(size_t)(right-left));
    }
    if(left<ui[0].left)ui[0].left=left;if(right>ui[0].right)ui[0].right=right;
    if(top<ui[0].top)ui[0].top=top;if(bottom>ui[0].bottom)ui[0].bottom=bottom;
}

static void rect_union(stronghold_rect *r,int left,int top,int right,int bottom)
{
    if(left>=right||top>=bottom)return;
    if(r->left>=r->right||r->top>=r->bottom){*r=(stronghold_rect){left,top,right,bottom};return;}
    if(left<r->left)r->left=left;if(top<r->top)r->top=top;
    if(right>r->right)r->right=right;if(bottom>r->bottom)r->bottom=bottom;
}

static int any_set(const uint8_t *p,size_t n)
{
#if defined(__aarch64__)
    while(n>=32){
        if(vmaxvq_u8(vorrq_u8(vld1q_u8(p),vld1q_u8(p+16))))return 1;
        p+=32;n-=32;
    }
#endif
    while(n>=8){uint64_t v;memcpy(&v,p,8);if(v)return 1;p+=8;n-=8;}
    for(size_t i=0;i<n;i++)if(p[i])return 1;
    return 0;
}

/* Copies the differing bytes of a row: the changed span [*first,*last), or 0. */
static int copy_changed(void *destination,const void *source,size_t n,size_t *first,size_t *last)
{
    uint8_t *d=destination;const uint8_t *s=source;
    size_t a=0,b=n;
    uint64_t x,y;
#if defined(__aarch64__)
    while(a+32<=n){
        uint8x16_t diff=vorrq_u8(veorq_u8(vld1q_u8(d+a),vld1q_u8(s+a)),
                                 veorq_u8(vld1q_u8(d+a+16),vld1q_u8(s+a+16)));
        if(vmaxvq_u8(diff))break;
        a+=32;
    }
#endif
    while(a+8<=n&&(memcpy(&x,d+a,8),memcpy(&y,s+a,8),x==y))a+=8;
    while(a<n&&d[a]==s[a])a++;
    if(a==n)return 0;
    while(b>=a+8&&(memcpy(&x,d+b-8,8),memcpy(&y,s+b-8,8),x==y))b-=8;
    while(d[b-1]==s[b-1])b--;
    memcpy(d+a,s+a,b-a);*first=a;*last=b;
    return 1;
}

/* Copies a captured layer for the composition and the touch thread. Only
 * rows that differ from the last published frame are written; they collect
 * in the layer's dirty rectangle until the GPU presenter uploads them. The
 * published coverage stays zero outside the shown bounds. */
static void publish_layer(int i)
{
    ui_layer *l=&ui[i];
    if(!l->pixels){l->shown=l->dirty=(stronghold_rect){0,0,0,0};return;}
    int w=requested_width,used=l->used&&l->left<l->right&&l->top<l->bottom;
    stronghold_rect area=l->shown;
    if(used)rect_union(&area,l->left,l->top,l->right,l->bottom);
    int first=area.bottom,last=area.top;
    size_t n=(size_t)(area.right-area.left),left=n,right=0,a,b;
    for(int y=area.top;y<area.bottom;y++){
        uint8_t *coverage=l->coverage+(size_t)y*w+area.left;
        int changed=0;
        if(used){
            uint16_t *pixels=l->pixels+(size_t)y*w+area.left;
            const uint16_t *source=GPTR(l->guest+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)(origin_x+area.left));
            const uint8_t *capture=l->capture+(size_t)y*w+area.left;
            /* Uncovered RGB never reaches composition or a filter's premultiplied input. */
            if(!any_set(capture,n)&&!any_set(coverage,n))continue;
            if(copy_changed(pixels,source,n*2,&a,&b)){changed=1;if(a/2<left)left=a/2;if((b+1)/2>right)right=(b+1)/2;}
            if(copy_changed(coverage,capture,n,&a,&b)){changed=1;if(a<left)left=a;if(b>right)right=b;}
        }else if(any_set(coverage,n)){memset(coverage,0,n);changed=1;left=0;right=n;}
        if(changed){if(y<first)first=y;last=y+1;}
    }
    if(first<last)rect_union(&l->dirty,area.left+(int)left,first,area.left+(int)right,last);
    l->shown=used?(stronghold_rect){l->left,l->top,l->right,l->bottom}:(stronghold_rect){0,0,0,0};
    if(used)l->fill=l->shown;
}

/* Keep the preferred original anchor unless its scaled content would leave
 * the frame. The shared scale first fits full dialogs and functional HUD,
 * excluding the repeating decorative strips at the sides of its 800px box. */
static float fit_anchor(int lo,int hi,int limit,float scale,double anchor)
{
    double first=anchor+(lo-anchor)*scale,last=anchor+(hi-anchor)*scale,shift=0;
    if(first<0)shift=-first;
    if(last+shift>limit)shift-=last+shift-limit;
    return fabsf(scale-1)<.00001f?(float)anchor:(float)(anchor+shift/(1-scale));
}
static void publish_geometry(void)
{
    float scale=ui_scale;
    if(top_overlay_used&&resource_fit_right>0){
        scale=fminf(scale,(float)requested_width/resource_fit_right);
        if(!published_hud_opaque)scale=fminf(scale,(float)requested_height/resource_fit_rows);
    }
    if(published_hud_opaque){
        scale=fminf(scale,requested_width/800.f);
        int span=requested_height-hud_top+(top_overlay_used?resource_fit_rows:0);
        if(span>0)scale=fminf(scale,(float)requested_height/span);
    }
    for(int i=1;i<SH_UI_LAYERS;i++){
        if(i==1&&UI_POWERS&&powers_scale>0)continue;
        stronghold_rect b=ui[i].shown;
        if(b.left<b.right&&b.top<b.bottom){
            scale=fminf(scale,(float)requested_width/(b.right-b.left));
            scale=fminf(scale,(float)requested_height/(b.bottom-b.top));
        }
    }
    published_ui_scale=scale;
    published_powers_scale=powers_scale;
    stronghold_rect powers=ui[1].shown;
    if(UI_POWERS&&powers_scale>0&&powers.left<powers.right&&powers.top<powers.bottom){
        published_powers_scale=fminf(powers_scale,fminf((float)requested_width/(powers.right-powers.left),
            (float)requested_height/(powers.bottom-powers.top)));
    }
    for(int i=0;i<SH_UI_LAYERS;i++){
        double ax,ay;layer_anchor(i,requested_width,requested_height,&ax,&ay);
        stronghold_rect b=ui[i].shown;
        if(i&&b.left<b.right&&b.top<b.bottom){
            float s=layer_scale(i);
            ax=fit_anchor(b.left,b.right,requested_width,s,ax);
            ay=fit_anchor(b.top,b.bottom,requested_height,s,ay);
        }
        published_anchor[i][0]=(float)ax;published_anchor[i][1]=(float)ay;
    }
}

void stronghold_display_ui_end(void)
{
    if(!ui_open)return;
    /* The engine presents the complete bottom128 from its persistent UI
     * surface; its existing keyed455340 pass has supplied the portrait above. */
    uint32_t hud=MEM32(GAME_SCREEN+0xd4),pitch=MEM32(GAME_SCREEN+0x40);
    int hud_opaque=0;
    if(pitch>=(uint32_t)requested_width*2&&void_vm_readable(hud,pitch*(uint32_t)requested_height)){
        /* Preserve exactly the unkeyed regions of original4707F0/4705E0. */
        copy_hud_rect(hud,pitch,0,requested_height-128,requested_width,requested_height);
        hud_opaque=1;
        unsigned mode=MEM32(GAME_SCREEN+0xc0);
        if((mode==3||mode==4)&&void_vm_readable(HUD_OFFSET,8)){
            int x=(int32_t)MEM32(HUD_OFFSET),y=(int32_t)MEM32(HUD_OFFSET+4);
            copy_hud_rect(hud,pitch,x+(mode==3?571:664),y+(mode==3?465:464),
                          x+(mode==3?697:792),y+(mode==3?591:592));
        }
    }
    pthread_mutex_lock(&display_lock);
    for(int i=0;i<SH_UI_LAYERS;i++)publish_layer(i);
    published_hud_opaque=hud_opaque&&ui[0].used;
    gpu_taken=0;
    MEM32(GAME_SCREEN+0xd8)=original_pixels;
    void_ddraw_redirect_pixels(original_surface,original_pixels);ui_open=0;stronghold_display_capture_active=0;
    published_width=requested_width;published_height=requested_height;
    published_source_width=source_width;published_source_height=source_height;
    published_raster_width=raster_width;published_raster_height=raster_height;
    publish_geometry();
    published_resource_rows=resource_rows;
    published_key=color_key;
    for(int i=0;i<SH_UI_LAYERS;i++)published_used[i]=ui[i].used;
    published_present_scale=world_zoom/(camera_last_native?.5f:1.f);
    published_world_zoom=world_zoom;
    published_active=frame_ready=1;
    pthread_mutex_unlock(&display_lock);
}

/* Save progress presents without a world/UI pass. Keep the last complete HUD
 * (including the scribe above the bottom128) and replace its closed dialog. */
void stronghold_display_progress(int x,int y,int bar)
{
    if(ui_open||!published_hud_opaque||!published_used[0]||
       published_width!=requested_width||published_height!=requested_height||
       x<0||y<0||x+301>requested_width||y+16>requested_height||
       !void_vm_readable(original_pixels,MAP_BYTES)||
       !allocate_layer(2))return;
    pthread_mutex_lock(&display_lock);
    /* The presenter may have removed raster text for its crisp glyph pass. */
    for(int i=0;i<2;i++)if(published_used[i]){ui[i].used=1;publish_layer(i);}
    ui_layer *l=&ui[2];stronghold_rect old=l->shown;
    for(int row=old.top;row<old.bottom;row++)
        memset(l->coverage+(size_t)row*requested_width+old.left,0,(size_t)(old.right-old.left));
    for(int row=y;bar&&row<y+16;row++){
        memcpy(l->pixels+(size_t)row*requested_width+x,
               GPTR(original_pixels+MAP_PITCH*(uint32_t)(origin_y+row)+2u*(uint32_t)(origin_x+x)),301*2);
        memset(l->coverage+(size_t)row*requested_width+x,255,301);
    }
    l->left=x;l->top=y;l->right=bar?x+301:x;l->bottom=bar?y+16:y;
    l->shown=(stronghold_rect){l->left,l->top,l->right,l->bottom};
    rect_union(&l->dirty,old.left,old.top,old.right,old.bottom);
    if(bar)rect_union(&l->dirty,x,y,x+301,y+16);
    for(int i=2;i<SH_UI_LAYERS;i++)published_used[i]=i==2&&bar;
    published_anchor[2][0]=requested_width*.5f;published_anchor[2][1]=requested_height*.5f;
    frame_ready=published_active=1;gpu_taken=0;
    pthread_mutex_unlock(&display_lock);
}

static void layer_point(int group,double x,double y,int *sx,int *sy)
{
    if(group==1&&!UI_POWERS)group=2;
    double ax=published_anchor[group][0],ay=published_anchor[group][1];
    if(group==0&&y<published_resource_rows*published_ui_scale){
        int bx=(int)floor((x-ax)/published_ui_scale+ax),by=(int)floor((y-ay)/published_ui_scale+ay);
        /* The bottom HUD draws after resources and may grow into their rows. */
        if(published_used[0]&&bx>=0&&bx<published_width&&by>=published_resource_rows&&by<published_height&&
           ui[0].coverage[(size_t)by*published_width+bx]){*sx=bx;*sy=by;return;}
        ax=ay=0;
    }
    *sx=(int)floor((x-ax)/layer_scale(group)+ax);
    *sy=(int)floor((y-ay)/layer_scale(group)+ay);
}

/* Raster operations above blend against a known RGB565 clear value. Remove
 * only that mathematical background contribution, using independent alpha.
 * In particular an opaque pixel equal to the clear value stays opaque. */
static uint16_t composite_pixel(uint16_t pixel,uint16_t below,unsigned alpha,uint16_t clear)
{return stronghold_composite_pixel(pixel,below,alpha,clear);}

int stronghold_display_ui_point(int group,float nx,float ny,int *gx,int *gy)
{
    if(group<0||group>=SH_UI_LAYERS)return 0;
    pthread_mutex_lock(&display_lock);
    double x,y;int inside=surface_point(nx,ny,&x,&y,NULL);
    if(inside){
        if(published_active)layer_point(group,x,y,gx,gy);
        else {*gx=(int)x;*gy=(int)y;}
        int w=published_width?published_width:requested_width;
        int h=published_height?published_height:requested_height;
        inside=*gx>=0&&*gy>=0&&*gx<w&&*gy<h;
    }
    pthread_mutex_unlock(&display_lock);return inside;
}

static float ui_delta(float delta,int horizontal)
{
    if(!isfinite(delta))return 0;
    pthread_mutex_lock(&display_lock);
    double x,y,fit,dy=0;
    if(surface_point(.5f,.5f,&x,&y,&fit)) {
        int pixels=horizontal?(surface_width?surface_width:(published_width?published_width:requested_width)):
                              (surface_height?surface_height:(published_height?published_height:requested_height));
        dy=delta*pixels/fit/(published_active?published_ui_scale:1);
    }
    pthread_mutex_unlock(&display_lock);return (float)dy;
}
float stronghold_display_ui_dy(float delta){return ui_delta(delta,0);}
float stronghold_display_ui_dx(float delta){return ui_delta(delta,1);}

int stronghold_display_world_point(float nx,float ny,int *gx,int *gy)
{
    pthread_mutex_lock(&display_lock);
    double x,y;int inside=surface_point(nx,ny,&x,&y,NULL);
    if(inside){*gx=(int)floor(x/(published_active?published_present_scale:1));
               *gy=(int)floor(y/(published_active?published_present_scale:1));}
    int region=inside?(published_active?1:2):0;
    pthread_mutex_unlock(&display_lock);return region;
}

int stronghold_display_map_point(float nx,float ny,int *gx,int *gy)
{
    pthread_mutex_lock(&display_lock);
    double x,y;int region=0;
    if(surface_point(nx,ny,&x,&y,NULL)){
        region=published_active?1:2;*gx=(int)x;*gy=(int)y;
        if(published_active){
            *gx=(int)floor(x/published_present_scale);*gy=(int)floor(y/published_present_scale);
            for(int order=SH_UI_LAYERS-1;order>=0;order--){
                int i=SH_UI_ORDER[order];if(!published_used[i])continue;
                int sx,sy;layer_point(i,x,y,&sx,&sy);
                if(sx>=0&&sy>=0&&sx<published_width&&sy<published_height&&
                   ui[i].coverage[(size_t)sy*published_width+sx]){
                    *gx=sx;*gy=sy;region=2;break;
                }
            }
        }
    }
    pthread_mutex_unlock(&display_lock);return region;
}

/* The selected construction button is lit in place: a soft, slowly pulsing
 * light over its own HUD pixels, following the HUD scale and bottom anchor. */
static void brighten_highlight(int width,int height)
{
    int hl=highlight_left,ht=highlight_top,hw=highlight_width,hh=highlight_height;
    if(hw<=0||hh<=0||ht<128||!published_used[0])return;
    double ax=width*.5,ay=height,s=published_ui_scale;
    int left=(int)floor(ax+(hl-ax)*s),right=(int)ceil(ax+(hl+hw-ax)*s);
    int top=(int)floor(ay+(ht-ay)*s),bottom=(int)ceil(ay+(ht+hh-ay)*s);
    if(left<0)left=0;if(top<0)top=0;if(right>width)right=width;if(bottom>height)bottom=height;
    if(left>=right||top>=bottom)return;
    double cx=(left+right)*.5,cy=(top+bottom)*.5,rx=fmax(1,(right-left)*.5),ry=fmax(1,(bottom-top)*.5);
    double pulse=.8+.2*sin((double)(void_monotonic_ns()%1400000000ull)/1400000000.0*6.283185307179586);
    for(int y=top;y<bottom;y++){
        int sy=(int)floor((y-ay)/s+ay);
        if(sy<0||sy>=height)continue;
        uint16_t *row=composed_pixels+(size_t)y*width;
        for(int x=left;x<right;x++){
            int sx=(int)floor((x-ax)/s+ax);
            if(sx<0||sx>=width||!ui[0].coverage[(size_t)sy*width+sx])continue;
            double dx=(x-cx)/rx,dy=(y-cy)/ry;
            double k=(.16+.30*fmax(0,1-(dx*dx+dy*dy)))*pulse;
            unsigned p=row[x],r=p>>11,g=(p>>5)&63,b=p&31;
            r+=(unsigned)((31-r)*k+.5);g+=(unsigned)((63-g)*k+.5);b+=(unsigned)((31-b)*k*.7+.5);
            row[x]=(uint16_t)(r<<11|g<<5|b);
        }
    }
}

const uint8_t *stronghold_display_compose(const uint8_t *pixels,int width,int height,int *pitch,int bpp)
{
    if(ui_open)return NULL;
    if(!frame_ready||width!=requested_width||height!=requested_height||bpp!=16){
        pthread_mutex_lock(&display_lock);published_active=0;pthread_mutex_unlock(&display_lock);
        return pixels;
    }
    if(frame_ready==2){*pitch=width*2;return (const uint8_t *)composed_pixels;}
    pthread_mutex_lock(&display_lock);
    int columns[4056];
    int rw=published_raster_width?published_raster_width:published_source_width;
    int rh=published_raster_height?published_raster_height:published_source_height;
    for(int x=0;x<width;x++)columns[x]=(int)((int64_t)(int)floor(x/published_present_scale)*rw/published_source_width);
    int previous_y=-1;
    for(int y=0;y<height;y++){
        int sy=(int)((int64_t)(int)floor(y/published_present_scale)*rh/published_source_height);
        if(sy>=rh)sy=rh-1;
        uint16_t *dst=composed_pixels+(size_t)y*width;
        if(sy==previous_y){memcpy(dst,dst-width,(size_t)width*2);continue;}
        previous_y=sy;
        const uint16_t *src=world_pixels+(size_t)sy*width;
        if(published_present_scale==1&&rw==width)memcpy(dst,src,(size_t)width*2);
        else for(int x=0;x<width;x++){
            int sx=columns[x];
            dst[x]=src[sx<rw?sx:rw-1];
        }
    }
    for(int order=0;order<SH_UI_LAYERS;order++){
        int i=SH_UI_ORDER[order];if(!published_used[i])continue;
        for(int part=0;part<(i==0?2:1);part++){
        float scale=layer_scale(i);
        double ax=published_anchor[i][0],ay=published_anchor[i][1];
        int source_top=ui[i].top,source_bottom=ui[i].bottom;
        /* The freebuild resource bar belongs to the top-left, unlike the bottom HUD. */
        if(i==0){if(!part){ax=ay=0;if(source_bottom>published_resource_rows)source_bottom=published_resource_rows;}
                 else if(source_top<published_resource_rows)source_top=published_resource_rows;}
        if(source_top>=source_bottom)continue;
        int left=(int)floor(ax+(ui[i].left-ax)*scale);
        int right=(int)ceil(ax+(ui[i].right-ax)*scale);
        int top=(int)floor(ay+(source_top-ay)*scale);
        int bottom=(int)ceil(ay+(source_bottom-ay)*scale);
        if(left<0)left=0;if(right>width)right=width;if(top<0)top=0;if(bottom>height)bottom=height;
        for(int x=left;x<right;x++)columns[x]=(int)floor((x-ax)/scale+ax);
        for(int y=top;y<bottom;y++){
            int sy=(int)floor((y-ay)/scale+ay);
            if(sy<0||sy>=height)continue;
            const uint16_t *src=ui[i].pixels+(size_t)sy*width;
            uint16_t *dst=composed_pixels+(size_t)y*width;
            for(int x=left;x<right;x++){
                int sx=columns[x];
                if(sx>=0&&sx<width){
                    unsigned a=ui[i].coverage[(size_t)sy*width+sx];
                    if(a)dst[x]=composite_pixel(src[sx],dst[x],a,published_key);
                }
            }
        }
        }
    }
    brighten_highlight(width,height);
    *pitch=width*2;
    frame_ready=2;
    pthread_mutex_unlock(&display_lock);return (const uint8_t *)composed_pixels;
}

/* The same composition as above, described for the GPU presenter. Group 0
 * is drawn in two parts, the complete resource overlay at the top-left
 * and the bottom HUD; layer pixels and dirty rectangles are handed over by
 * pointer and taken (cleared) here, on the guest thread that writes them. */
int stronghold_display_frame(stronghold_frame *f,int width,int height,int bpp)
{
    if(ui_open)return -1;
    pthread_mutex_lock(&display_lock);
    if(!frame_ready||width!=requested_width||height!=requested_height||bpp!=16){
        published_active=0;pthread_mutex_unlock(&display_lock);
        return 0;
    }
    memset(f,0,sizeof *f);
    int rw=published_raster_width?published_raster_width:published_source_width;
    int rh=published_raster_height?published_raster_height:published_source_height;
    f->width=width;f->height=height;f->world=world_pixels;
    f->raster_width=rw;f->raster_height=rh;
    f->world_scale_x=(float)(published_present_scale*published_source_width/rw);
    f->world_scale_y=(float)(published_present_scale*published_source_height/rh);
    f->ui_scale=published_ui_scale;f->key=published_key;
    f->fresh=!gpu_taken;gpu_taken=1;
    f->hud_opaque=published_hud_opaque;
    for(int i=0;i<SH_UI_LAYERS;i++){
        f->layer[i].pixels=ui[i].pixels;f->layer[i].coverage=ui[i].coverage;
        f->layer[i].used=published_used[i]&&ui[i].shown.left<ui[i].shown.right&&ui[i].shown.top<ui[i].shown.bottom;
        f->layer[i].bounds=ui[i].shown;f->layer[i].dirty=ui[i].dirty;
        ui[i].dirty=(stronghold_rect){0,0,0,0};
    }
    if(f->layer[0].used){
        f->part[f->parts++]=(stronghold_part){0,0,published_resource_rows,0,0};
        f->part[f->parts++]=(stronghold_part){0,published_resource_rows,height,width*.5f,(float)height};
    }
    for(int order=1;order<SH_UI_LAYERS;order++){
        int i=SH_UI_ORDER[order];
        if(f->layer[i].used)f->part[f->parts++]=(stronghold_part){i,0,height,published_anchor[i][0],published_anchor[i][1],layer_scale(i)};
    }
    if(highlight_width>0&&highlight_height>0&&highlight_top>=published_resource_rows&&f->layer[0].used)
        f->highlight=(stronghold_rect){highlight_left,highlight_top,highlight_left+highlight_width,highlight_top+highlight_height};
    pthread_mutex_unlock(&display_lock);return 1;
}

#ifdef STRONGHOLD_DISPLAY_SELFTEST
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
void void_log(const char *fmt, ...) { (void)fmt; }
static int selftest_feedback;
void stronghold_feedback(int bits) { selftest_feedback |= bits; }
void stronghold_perf_frame_begin(void) {}
uint64_t void_monotonic_ns(void) { return 0; }
uint32_t void_vm_alloc(uint32_t p,uint32_t n,uint32_t t,uint32_t prot)
{(void)p;(void)n;(void)t;(void)prot;return 0;}
uint32_t void_ddraw_redirect_pixels(uint32_t s,uint32_t p){(void)s;return p;}
void void_input_set_screen_size(int w, int h) { (void)w; (void)h; }
int void_vm_writable(uint32_t va, uint32_t count)
{
    return (va >= GAME_SCREEN && (uint64_t)va + count <= GAME_SCREEN + 0x100) ||
           (va >= 0x10000000 && (uint64_t)va + count <= 0x10400000) ||
           (va >= 0x11000000 && (uint64_t)va + count <= 0x1A000000);
}
int void_vm_readable(uint32_t va,uint32_t count){return void_vm_writable(va,count);}

static uint16_t test_world(int x,int y)
{return (uint16_t)((((x/8)&31)<<11)|(((y/8)&63)<<5)|((x+y)&31));}

static void clear_batch_selfcheck(void)
{
    const uint32_t context=0x12fff000;
    origin_x=3;origin_y=2;stronghold_display_capture_active=1;
    for(int mode=0;mode<2;mode++){
        for(int y=0;y<6;y++)for(int x=0;x<7;x++){
            ui[2].capture[(size_t)y*requested_width+x]=200;
            MEM16(ui[2].guest+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)(origin_x+x))=0x1234;
            MEM16(hud_pixels+hud_pitch*(uint32_t)y+2u*(uint32_t)x)=0x2104;
        }
        MEM32(context+4)=mode?ui[2].guest:hud_pixels;
        MEM32(context+8)=mode?MAP_PITCH:hud_pitch;
        int left=mode?origin_x-1:1,top=mode?origin_y-1:1;
        int right=mode?origin_x+2:4,bottom=mode?origin_y+2:3;
        MEM32(context+0x30)=left;MEM32(context+0x34)=top;
        MEM32(context+0x38)=right;MEM32(context+0x3c)=bottom;
        stronghold_display_clear_begin(context);
        assert(stronghold_display_clear_batch_active&&!stronghold_display_capture_active);
        stronghold_display_clear_clipped(1);
        /* Exactly as original471640: top advances and height is destroyed. */
        MEM32(context+0x34)=bottom+1;MEM32(context+0x2c)=UINT32_MAX;
        for(int y=top;y<=bottom;y++)for(int x=left;x<=right;x++)
            MEM16(MEM32(context+4)+MEM32(context+8)*(uint32_t)y+2u*(uint32_t)x)=0xf81f;
        stronghold_display_clear_scope(0);
        assert(!stronghold_display_clear_batch_active&&stronghold_display_capture_active&&!raster_clear_depth);
        for(int y=0;y<6;y++)for(int x=0;x<7;x++){
            int cleared=mode?(x<=2&&y<=2):(x>=1&&x<=4&&y>=1&&y<=3);
            assert(ui[2].capture[(size_t)y*requested_width+x]==(cleared?0:200));
            assert(MEM16(ui[2].guest+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)(origin_x+x))==(cleared?0xf81f:0x1234));
        }
    }
    uint32_t pixel=ui[2].guest+MAP_PITCH*(uint32_t)(origin_y+1)+2u*(uint32_t)(origin_x+1);
    /* Same RGB key, ordinary opaque store: no transparency heuristic. */
    stronghold_display_written(pixel,2);
    assert(MEM16(pixel)==0xf81f&&ui[2].capture[requested_width+1]==255);
    stronghold_display_clear_begin(context);stronghold_display_clear_clipped(0);
    stronghold_display_clear_scope(0);
    assert(MEM16(pixel)==0xf81f&&ui[2].capture[requested_width+1]==255);
    assert(stronghold_display_capture_active&&!stronghold_display_clear_batch_active);
    origin_x=origin_y=0;stronghold_display_capture_active=0;
    puts("batched semantic clear: original clipped D4/D8 rectangles, origin/stride, failed clip, opaque identical RGB PASS");
}

static void bulk_rows_selfcheck(void)
{
    origin_x=3;origin_y=2;
    const uint16_t colors[]={0,0x07ff,0xf81f,0x39e7};
    for(int d4=0;d4<2;d4++){
        int y=8+d4,left=12,count=200;
        uint32_t destination=ui[2].guest+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)(origin_x+left);
        uint32_t source=d4?hud_pixels+hud_pitch*(uint32_t)y+2u*(uint32_t)left:destination;
        for(int x=0;x<count;x++){
            MEM16(source+2u*(uint32_t)x)=colors[x%4];
            ui[2].capture[(size_t)y*requested_width+left+x]=127;
        }
        stronghold_display_written(source,(unsigned)count*2);
        for(int x=0;x<count;x++){
            assert(ui[2].capture[(size_t)y*requested_width+left+x]==255);
            assert(MEM16(destination+2u*(uint32_t)x)==colors[x%4]);
            assert(composite_pixel(colors[x%4],0x07e0,255,0xf81f)==colors[x%4]);
        }
        /* Stale half-alpha would distort the thumbnail toward neon green. */
        assert(composite_pixel(0x39e7,0x07e0,127,0xf81f)!=0x39e7);
    }
    for(int x=0;x<7;x++)ui[2].capture[12*(size_t)requested_width+x]=77;
    uint32_t left=ui[2].guest+MAP_PITCH*(uint32_t)(origin_y+12)+2u*(uint32_t)(origin_x-2);
    stronghold_display_written(left,16);
    for(int x=0;x<7;x++)assert(ui[2].capture[12*(size_t)requested_width+x]==(x<6?255:77));
    size_t end=13*(size_t)requested_width+requested_width-2;
    ui[2].capture[end]=ui[2].capture[end+1]=77;ui[2].capture[14*(size_t)requested_width]=77;
    uint32_t right=ui[2].guest+MAP_PITCH*(uint32_t)(origin_y+13)+2u*(uint32_t)(origin_x+requested_width-2);
    stronghold_display_written(right,8);
    assert(ui[2].capture[end]==255&&ui[2].capture[end+1]==255&&ui[2].capture[14*(size_t)requested_width]==77);
    uint32_t start=ui[2].guest+MAP_PITCH*(uint32_t)(origin_y+14)+2u*(uint32_t)origin_x;
    stronghold_display_written(start,3);stronghold_display_written(start+1,2);
    stronghold_display_written(ui[2].guest+MAP_PITCH*(uint32_t)(origin_y+13)+MAP_PITCH-2,4);
    assert(ui[2].capture[14*(size_t)requested_width]==77);
    origin_x=origin_y=0;
    puts("bulk UI coverage: opaque200-pixel D4/D8 rows, original RGB565/green regression, viewport clips and invalid spans PASS");
}

/* GPU presenter interface: filter settings, loupe state, changed-row
 * publication and the frame description (parts, anchors, scales). */
static void gpu_frame_selfcheck(void)
{
    int w,u,d,c;float s,nx,ny,cx,cy,r;
    stronghold_display_set_filters(9,-1,NAN,1,0);stronghold_display_filters(&w,&u,&s,&d,&c);
    assert(w==DEFAULT_WORLD_FILTER&&u==DEFAULT_UI_FILTER&&s==0&&d==1&&c==0);
    stronghold_display_set_filters(SH_FILTER_FSR,SH_FILTER_BICUBIC,.5f,0,1);stronghold_display_filters(&w,&u,&s,&d,&c);
    assert(w==SH_FILTER_FSR&&u==SH_FILTER_BICUBIC&&s==.5f&&!d&&c);
    assert(stronghold_display_image_filter(&s)==2&&s==.5f); /* ES 2 fallback: FSR becomes bicubic */
    stronghold_display_set_image_filter(1,.75f);stronghold_display_filters(&w,&u,&s,NULL,NULL);
    assert(w==SH_FILTER_PIXELS&&u==SH_FILTER_PIXELS&&s==.75f);
    stronghold_display_set_filters(DEFAULT_WORLD_FILTER,DEFAULT_UI_FILTER,.25f,0,1);
    stronghold_display_magnifier(1,.25f,1.5f);assert(stronghold_display_loupe(&nx,&ny)&&nx==.25f&&ny==1);
    stronghold_display_magnifier(1,NAN,.5f);assert(!stronghold_display_loupe(&nx,&ny));
    stronghold_display_magnifier(1,.5f,.5f);stronghold_display_magnifier(0,0,0);assert(!stronghold_display_loupe(&nx,&ny));
    stronghold_loupe_place(1170,540,2340,1080,1,&cx,&cy,&r);assert(cx<1170&&cy<540&&hypotf(cx-1170,cy-540)>r);
    stronghold_loupe_place(10,1070,2340,1080,1,&cx,&cy,&r);assert(cx>10&&cy<1070&&cx>=r&&cy<=1080-r);
    {   /* The changed span of a row, across word boundaries. */
        uint8_t d[37],s[37];size_t a,b;
        for(int i=0;i<37;i++)for(int j=i;j<37;j++){
            memset(d,7,37);memset(s,7,37);s[i]=9;s[j]=9;
            assert(copy_changed(d,s,37,&a,&b)&&a==(size_t)i&&b==(size_t)j+1&&!memcmp(d,s,37));
        }
        assert(!copy_changed(d,s,37,&a,&b));
    }
    /* Only changed rows are published as dirty; an unused layer clears its coverage. */
    const int width=requested_width,height=requested_height;
    ui[2].guest=0x11000000;origin_x=origin_y=0;
    for(int y=0;y<height;y++)for(int x=0;x<width;x++){
        MEM16(ui[2].guest+MAP_PITCH*(uint32_t)y+2u*(uint32_t)x)=y>=100&&y<110&&x>=200&&x<300?0x1234:0xf81f;
        ui[2].capture[(size_t)y*width+x]=y>=100&&y<110&&x>=200&&x<300?255:0;
        ui[2].coverage[(size_t)y*width+x]=0;
    }
    ui[2].used=1;ui[2].left=200;ui[2].top=100;ui[2].right=300;ui[2].bottom=110;
    ui[2].shown=ui[2].dirty=(stronghold_rect){0,0,0,0};
    publish_layer(2);
    assert(ui[2].dirty.left==200&&ui[2].dirty.top==100&&ui[2].dirty.right==300&&ui[2].dirty.bottom==110);
    assert(ui[2].pixels[105*width+250]==0x1234&&ui[2].coverage[105*width+250]==255&&ui[2].coverage[99*width+250]==0);
    ui[2].dirty=(stronghold_rect){0,0,0,0};publish_layer(2);assert(ui[2].dirty.right==0);
    MEM16(ui[2].guest+MAP_PITCH*105u+500u)=0x4321;publish_layer(2);
    assert(ui[2].dirty.top==105&&ui[2].dirty.bottom==106&&ui[2].dirty.left==250&&ui[2].dirty.right==251);
    assert(ui[2].pixels[105*width+250]==0x4321);
    ui[2].dirty=(stronghold_rect){0,0,0,0};ui[2].used=0;publish_layer(2);
    assert(ui[2].dirty.top==100&&ui[2].dirty.bottom==110&&!ui[2].coverage[105*width+250]&&ui[2].shown.right==0);
    ui[2].used=1;publish_layer(2);
    /* The frame description: compose's anchors, scales, taken dirty rectangles. */
    ui[0].shown=(stronghold_rect){0,8,width,height};ui[0].used=1;
    published_used[0]=1;published_used[1]=0;published_used[2]=1;
    published_present_scale=2;published_source_width=640;published_source_height=360;
    published_raster_width=480;published_raster_height=270;published_ui_scale=1.5f;published_key=0xf81f;
    published_hud_opaque=1;frame_ready=1;gpu_taken=0;
    stronghold_display_highlight(600,600,40,30);
    stronghold_frame f;
    ui_open=1;assert(stronghold_display_frame(&f,width,height,16)<0);ui_open=0;
    assert(stronghold_display_frame(&f,width,height,16)==1&&f.fresh&&f.hud_opaque);
    assert(f.parts==3&&f.part[0].layer==0&&f.part[0].bottom==128&&f.part[0].anchor_x==0&&f.part[0].anchor_y==0);
    assert(f.part[1].top==128&&f.part[1].anchor_x==width*.5f&&f.part[1].anchor_y==height);
    assert(f.part[2].layer==2&&f.part[2].anchor_x==width*.5f&&f.part[2].anchor_y==height*.5f);
    assert(f.raster_width==480&&fabsf(f.world_scale_x-640.f/240)<1e-5f&&fabsf(f.world_scale_y-720.f/270)<1e-5f);
    assert(f.ui_scale==1.5f&&f.key==0xf81f&&f.layer[2].used&&!f.layer[1].used);
    assert(f.layer[2].bounds.left==200&&f.layer[2].dirty.top==100&&!ui[2].dirty.right);
    assert(f.highlight.left==600&&f.highlight.bottom==630);
    assert(stronghold_display_frame(&f,width,height,16)==1&&!f.fresh&&f.layer[2].dirty.right==0);
    stronghold_display_highlight(600,100,40,20);assert(stronghold_display_frame(&f,width,height,16)==1&&f.highlight.right==0);
    published_active=1;assert(stronghold_display_frame(&f,width-8,height,16)==0&&!published_active);
    /* A save runs after frame_begin, without repainting world or the scribe. */
    original_pixels=0x16000000;ui[0].guest=0x14000000;
    published_width=width;published_height=height;
    ui[0].pixels[(height-180)*width+900]=0;
    ui[0].capture[(height-180)*width+900]=255;
    MEM16(ui[0].guest+MAP_PITCH*(uint32_t)(height-180)+1800)=0x1234;
    MEM16(original_pixels+MAP_PITCH*180u+1000)=0x07e0;
    stronghold_display_frame_begin();
    stronghold_display_progress(0,0,0);
    assert(!published_used[2]&&!ui[2].coverage[105*width+250]);
    stronghold_display_progress(500,180,1);
    assert(stronghold_display_frame(&f,width,height,16)==1&&f.fresh&&f.ui_scale==1.5f);
    assert(f.layer[0].used&&f.layer[0].pixels[(height-180)*width+900]==0x1234);
    assert(f.layer[0].coverage[(height-180)*width+900]==255);
    assert(f.layer[2].bounds.top==180&&f.layer[2].bounds.right==801);
    assert(f.layer[2].pixels[180*width+500]==0x07e0&&f.layer[2].coverage[180*width+500]==255);
    assert(!f.layer[2].coverage[105*width+250]);
    stronghold_display_progress(-1,180,1);assert(ui[2].shown.left==500);
    puts("save progress: complete scribe/HUD, stable scale, closed modal replaced, bounds PASS");
    stronghold_display_highlight(0,0,0,0);
    ui[0].used=ui[2].used=0;ui[2].guest=0;frame_ready=0;published_used[0]=published_used[2]=0;
    published_raster_width=published_raster_height=0;published_present_scale=1;published_ui_scale=1;
    puts("GPU frame: filter settings, loupe state/placement, changed rows only, compose anchors and scales PASS");
}

static void resource_selfcheck(void)
{
    stronghold_display_configure(1280,720);assert(allocate_frames());
    uint32_t item=0x10001000;
#ifdef STRONGHOLD_CLASSIC
    MEM32(item+0x18)=0x00433F40u;
#else
    MEM32(item+0x18)=0x00434150u;
#endif
    ui_open=1;current_group=0;ui[0].guest=0x11000000;origin_x=origin_y=0;resource_rows=128;
    stronghold_display_resource_overlay(item,1);assert(resource_capture);
    stronghold_display_written(ui[0].guest+304u*MAP_PITCH+40,2);
    int fitted=resource_fit_right;
    stronghold_display_resource_animation(1);
    stronghold_display_written(ui[0].guest+100u*MAP_PITCH+2400,64);
    stronghold_display_resource_animation(0);
    assert(resource_capture==1&&resource_fit_right==fitted);
    stronghold_display_resource_overlay(item,0);assert(!resource_capture&&resource_rows==308);
    stronghold_display_written(ui[0].guest+580u*MAP_PITCH+40,2);assert(resource_rows==308);
    MEM32(item+0x18)=0;stronghold_display_resource_overlay(item,1);assert(!resource_capture);
    ui_open=0;published_resource_rows=resource_rows;published_width=1280;published_height=720;
    published_source_width=published_raster_width=1280;published_source_height=published_raster_height=720;
    published_present_scale=1;published_active=1;published_key=0xf81f;
    for(int i=0;i<SH_UI_LAYERS;i++){memset(ui[i].coverage,0,1280u*720);ui[i].used=published_used[i]=i==0;}
    for(int i=0;i<1280*720;i++)world_pixels[i]=0x2104;
    const int rows[]={20,100,140,180,240,300};
    for(int r=0;r<6;r++)for(int y=rows[r];y<rows[r]+6;y++)for(int x=10;x<60;x++){
        ui[0].pixels[y*1280+x]=(uint16_t)(0xf800+r);ui[0].coverage[y*1280+x]=255;
    }
    for(int y=530;y<540;y++)for(int x=800;x<810;x++){ui[0].pixels[y*1280+x]=0x07e0;ui[0].coverage[y*1280+x]=255;}
    ui[0].left=0;ui[0].top=0;ui[0].right=1280;ui[0].bottom=720;
    ui[0].shown=(stronghold_rect){0,0,1280,720};stronghold_display_surface_size(1280,720);
    for(int percent=75;percent<=200;percent+=5){
        published_ui_scale=percent/100.f;frame_ready=1;int pitch=2560;
        const uint16_t *out=(const uint16_t *)stronghold_display_compose(NULL,1280,720,&pitch,16);
        for(int r=0;r<6;r++){
            float x=20.5f*published_ui_scale,y=(rows[r]+2.5f)*published_ui_scale;
            assert(out[(int)y*1280+(int)x]==(uint16_t)(0xf800+r));
            int gx,gy;assert(stronghold_display_map_point(x/1280,y/720,&gx,&gy)==2&&gx==20&&gy==rows[r]+2);
            float vx,vy,scale;assert(stronghold_display_layer_view(0,20.5f,rows[r]+2.5f,&vx,&vy,&scale));
            assert(fabsf(vx-x)<.01f&&fabsf(vy-y)<.01f&&fabsf(scale-published_ui_scale)<.01f);
        }
        stronghold_frame f;assert(stronghold_display_frame(&f,1280,720,16));
        assert(f.part[0].bottom==308&&f.part[1].top==308);
        float hx=640+(802.5f-640)*published_ui_scale,hy=720+(532.5f-720)*published_ui_scale;
        int gx,gy;assert(stronghold_display_map_point(hx/1280,hy/720,&gx,&gy)==2&&gx==802&&gy==532);
    }
    published_resource_rows=resource_rows=128;published_ui_scale=1;ui[0].guest=0;
    puts("resources: actual callback coverage beyond128, six ordered rows, forward/inverse/crisp-text/GPU bounds at75..200% PASS");
}

static void notice_selfcheck(void)
{
    const int w=1280,h=720;
    uint32_t item=0x10001000;
    stronghold_display_configure(w,h);assert(allocate_frames());
    ui_open=stronghold_display_capture_active=1;current_group=0;
    ui[0].guest=0x11000000;ui[3].guest=0x12000000;origin_x=origin_y=0;
    MEM32(item+0x18)=0x004AFFA0;MEM32(item+0x1c)=6;
    stronghold_display_resource_overlay(item,1);
    assert(current_group==3&&!resource_capture&&overlay_previous_group==0);
    for(int y=520;y<540;y++){
        for(int x=10;x<530;x++)MEM16(ui[3].guest+MAP_PITCH*(uint32_t)y+2u*(uint32_t)x)=0xffff;
        stronghold_display_written(ui[3].guest+MAP_PITCH*(uint32_t)y+20,1040);
    }
    stronghold_display_resource_overlay(item,0);assert(current_group==0&&overlay_previous_group==-1);
    MEM32(item+0x1c)=9;ui[5].guest=0x13000000;stronghold_display_resource_overlay(item,1);assert(current_group==5);
    stronghold_display_resource_overlay(item,0);ui_open=stronghold_display_capture_active=0;
    publish_layer(3);
    for(int i=0;i<SH_UI_LAYERS;i++)published_used[i]=i==3;
    for(int i=0;i<w*h;i++)world_pixels[i]=0x2104;
    published_width=w;published_height=h;published_source_width=published_raster_width=w;
    published_source_height=published_raster_height=h;published_present_scale=1;published_active=1;
    stronghold_display_surface_size(w,h);
    for(int percent=75;percent<=200;percent+=5){
        published_ui_scale=percent/100.f;frame_ready=1;int pitch=w*2;
        const uint16_t *out=(const uint16_t *)stronghold_display_compose(NULL,w,h,&pitch,16);
        for(int lx=12;lx<=528;lx+=258){
            float x=(lx+.5f)*published_ui_scale,y=h+(525.5f-h)*published_ui_scale;
            assert(x>=0&&x<w&&y>=0&&y<h&&out[(int)y*w+(int)x]==0xffff);
            int gx,gy;assert(stronghold_display_map_point(x/w,y/h,&gx,&gy)==2&&gx==lx&&gy==525);
            float vx,vy,scale;assert(stronghold_display_layer_view(3,lx+.5f,525.5f,&vx,&vy,&scale));
            assert(fabsf(vx-x)<.01f&&fabsf(vy-y)<.01f&&fabsf(scale-published_ui_scale)<.01f);
        }
        stronghold_frame f;assert(stronghold_display_frame(&f,w,h,16));
        assert(f.parts==1&&f.part[0].layer==3&&f.part[0].anchor_x==0&&f.part[0].anchor_y==h);
    }
    ui[3].used=0;publish_layer(3);assert(!ui[3].coverage[525*w+12]&&!ui[3].shown.right);
    published_used[3]=0;published_ui_scale=1;ui[0].guest=ui[3].guest=0;
    puts("notices: original left+bottom flags, scoped capture/restoration, first+last columns visible, CPU/inverse/crisp/GPU75..200%, expired coverage clears PASS");
}

static void overlay_geometry_selfcheck(void)
{
    const int sizes[][2]={{800,600},{1280,720},{1920,720}};
    const unsigned flags[]={0,1,2,5,6,8,9,2};
    const int groups[]={2,4,0,0,3,6,5,7};
    const uint32_t item=0x10001000;
    for(int size=0;size<3;size++){
        int w=sizes[size][0],h=sizes[size][1];
        stronghold_display_configure(w,h);assert(allocate_frames());
        stronghold_display_surface_size(w,h);
        for(int i=0;i<SH_UI_LAYERS;i++)ui[i].guest=0x11000000u+0x1100000u*(unsigned)i;
        for(int i=0;i<w*h;i++)world_pixels[i]=0x2104;
        for(int percent=75;percent<=200;percent+=25)for(int kind=0;kind<8;kind++){
            stronghold_rect b=kind==0?(stronghold_rect){w/2-264,50,w/2+264,h-50}:
                kind==1?(stronghold_rect){w/2-264,10,w/2+264,80}:
                kind==2?(stronghold_rect){w/2-80,h-180,w/2+80,h-160}:
                kind==3?(stronghold_rect){4,140,164,160}:
                kind==4?(stronghold_rect){10,h-180,530,h-160}:
                kind==5?(stronghold_rect){w-140,40,w-10,h-200}:
                kind==6?(stronghold_rect){w-90,9,w-10,30}:
                        (stronghold_rect){w/2-140,h-210,w/2+140,h-150};
            stronghold_display_frame_begin();assert(select_group(0));
            ui_open=stronghold_display_capture_active=1;origin_x=origin_y=0;
            MEM32(GAME_SCREEN+0x40)=0;original_surface=1;original_pixels=0;
            source_width=raster_width=w;source_height=raster_height=h;world_zoom=1;camera_last_native=0;
            ui_scale=percent/100.f;MEM32(item+8)=kind==7?21:0;MEM32(item+0x18)=0x004B0130;MEM32(item+0x1c)=flags[kind];
            stronghold_display_resource_overlay(item,1);assert(current_group==groups[kind]);
            for(int y=b.top;y<b.bottom;y++){
                for(int x=b.left;x<b.right;x++)MEM16(ui[current_group].guest+MAP_PITCH*(uint32_t)y+2u*(uint32_t)x)=0xffff;
                stronghold_display_written(ui[current_group].guest+MAP_PITCH*(uint32_t)y+2u*(uint32_t)b.left,2u*(unsigned)(b.right-b.left));
            }
            stronghold_display_resource_overlay(item,0);assert(current_group==0);
            stronghold_display_ui_end();if(flags[kind]==5)assert(published_resource_rows==b.bottom+3);
            float x0,y0,x1,y1,s;
            assert(stronghold_display_layer_view(groups[kind],b.left,b.top,&x0,&y0,&s));
            assert(stronghold_display_layer_view(groups[kind],b.right,b.bottom,&x1,&y1,&s));
            assert(x0>=-.01f&&y0>=-.01f&&x1<=w+.01f&&y1<=h+.01f);
            float cx=(b.left+b.right)*.5f+.5f,cy=(b.top+b.bottom)*.5f+.5f,x,y;
            assert(stronghold_display_layer_view(groups[kind],cx,cy,&x,&y,&s));
            int gx,gy;assert(stronghold_display_map_point(x/w,y/h,&gx,&gy)==2&&gx==(int)cx&&gy==(int)cy);
            int pitch=w*2;const uint16_t *out=(const uint16_t *)stronghold_display_compose(NULL,w,h,&pitch,16);
            assert(out[(int)y*w+(int)x]==0xffff);
            stronghold_frame f;assert(stronghold_display_frame(&f,w,h,16));int matched=0;
            for(int p=0;p<f.parts;p++)if(f.part[p].layer==groups[kind]&&cy>=f.part[p].top&&cy<f.part[p].bottom){
                assert(fabsf(f.part[p].anchor_x+(cx-f.part[p].anchor_x)*f.ui_scale-x)<.01f);
                assert(fabsf(f.part[p].anchor_y+(cy-f.part[p].anchor_y)*f.ui_scale-y)<.01f);matched++;
            }
            assert(matched==1);
        }
        for(int i=0;i<SH_UI_LAYERS;i++)ui[i].shown=(stronghold_rect){0,0,0,0};
        published_hud_opaque=top_overlay_used=1;hud_top=h-200;resource_rows=resource_fit_rows=308;ui_scale=2;publish_geometry();
        assert(800*published_ui_scale<=w+.01f&&508*published_ui_scale<=h+.01f);
        ui_scale=1.2f;resource_fit_rows=128;resource_fit_right=160;hud_top=h-200;publish_geometry();
        float fixed=published_ui_scale;resource_rows=h-1;top_overlay_right=w;publish_geometry();
        assert(fabsf(published_ui_scale-fixed)<.00001f);
    }
    puts("overlay geometry: seven registered flag classes, narrow/wide75..200%, all edges visible, CPU/inverse/crisp/GPU agree, every left-top callback extends band, full HUD/resources fit PASS");
}

static void composition_selfcheck(void)
{
    stronghold_display_configure(1280,720);assert(allocate_frames());
    for(int i=0;i<SH_UI_LAYERS;i++){
        assert(allocate_layer(i));double ax,ay;layer_anchor(i,1280,720,&ax,&ay);
        published_anchor[i][0]=(float)ax;published_anchor[i][1]=(float)ay;
    }
    published_width=1280;published_height=720;published_source_width=1280;published_source_height=720;
    published_active=1;published_present_scale=1;published_ui_scale=1;color_key=published_key=0xf81f;
    stronghold_display_surface_size(1560,720);
    for(int y=0;y<720;y++)for(int x=0;x<1280;x++){
        size_t at=(size_t)y*1280+x;world_pixels[at]=test_world(x,y);
        for(int i=0;i<SH_UI_LAYERS;i++){ui[i].pixels[at]=color_key;ui[i].coverage[at]=0;}
        if(x>=240&&x<1040&&y>=592){ui[0].pixels[at]=0xffe0;ui[0].coverage[at]=255;}
        if(x>=8&&x<180&&y>=8&&y<32){ui[0].pixels[at]=0xf800;ui[0].coverage[at]=255;}
        if(x>=264&&x<328&&y>=528&&y<592){ui[0].pixels[at]=0xffe0;ui[0].coverage[at]=255;}
        if(x>=1225&&y>=56&&y<376){ui[1].pixels[at]=0x001f;ui[1].coverage[at]=255;}
        if(x>=480&&x<800&&y>=200&&y<500){ui[2].pixels[at]=0x07e0;ui[2].coverage[at]=255;}
    }
    for(int i=0;i<SH_UI_LAYERS;i++){
        ui[i].used=published_used[i]=1;ui[i].left=ui[i].top=0;ui[i].right=1280;ui[i].bottom=720;
    }
    if(!UI_POWERS)ui[1].used=published_used[1]=0;
    for(int scale=75;scale<=200;scale+=5){
        published_ui_scale=(float)scale/100;frame_ready=1;int pitch=2560;
        assert(fabsf(stronghold_display_ui_dy(.1f)-72/published_ui_scale)<.001f);
        assert(fabsf(stronghold_display_ui_dx(.1f)-156/published_ui_scale)<.001f);
        stronghold_display_surface_size(3120,1440);
        assert(fabsf(stronghold_display_ui_dy(.1f)-72/published_ui_scale)<.001f);
        assert(fabsf(stronghold_display_ui_dx(.1f)-156/published_ui_scale)<.001f);
        stronghold_display_surface_size(1560,720);
        assert(stronghold_display_ui_dy(NAN)==0);
        assert(stronghold_display_ui_dx(NAN)==0);
        const uint16_t *out=(const uint16_t *)stronghold_display_compose((uint8_t *)world_pixels,1280,720,&pitch,16);
        assert(out&&pitch==2560&&out[680*1280+640]==0xffe0);
        assert(out[20*1280+50]==0xf800);
        assert(out[360*1280+640]==0x07e0);
        if(UI_POWERS)assert(out[100*1280+1260]==0x001f);
        int gx,gy;
        assert(stronghold_display_map_point((50.5f+140)/1560,20.5f/720,&gx,&gy)==2);
        assert(gx==(int)floor(50.5/published_ui_scale)&&gy==(int)floor(20.5/published_ui_scale));
        assert(stronghold_display_map_point((float)(640.5+140)/1560,(float)680.5/720,&gx,&gy)==2);
        int sx,sy;layer_point(0,640.5,680.5,&sx,&sy);assert(gx==sx&&gy==sy);
        assert(stronghold_display_map_point(50.f/1560,.5f,&gx,&gy)==0);
        assert(stronghold_display_world_point((float)(100.5+140)/1560,(float)100.5/720,&gx,&gy)==1);
        assert(gx==100&&gy==100);
        if(scale==75)assert(out[710*1280+10]==test_world(10,710));
    }
    published_ui_scale=.75f;published_present_scale=2;published_source_width=640;published_source_height=360;frame_ready=1;
    int pitch=2560;const uint16_t *out=(const uint16_t *)stronghold_display_compose((uint8_t *)world_pixels,1280,720,&pitch,16);
    assert(out[100*1280+100]==test_world(50,50));
    assert(out[680*1280+640]==0xffe0); /* World zoom leaves HUD size unchanged. */
    int gx,gy;assert(stronghold_display_world_point((float)(100.5+140)/1560,(float)100.5/720,&gx,&gy)==1);
    assert(gx==50&&gy==50);
    published_ui_scale=published_present_scale=1;
    ui[0].pixels[680*1280+640]=published_key;ui[0].coverage[680*1280+640]=0;
    assert(stronghold_display_map_point((float)(640.5+140)/1560,(float)680.5/720,&gx,&gy)==1);
    assert(stronghold_display_ui_point(0,(float)(640.5+140)/1560,(float)680.5/720,&gx,&gy));
    assert(gx==640&&gy==680); /* A transparent hole still reaches the real UI's first refusal. */
    published_ui_scale=.75f;
    assert(stronghold_display_ui_point(1,(float)(640.5+140)/1560,(float)360.5/720,&gx,&gy));
    if(UI_POWERS)assert(gx==427&&gy==462);
    else assert(gx==640&&gy==360);
    world_zoom=1;unsnapped_zoom=1;pending_x=pending_y=0;
    stronghold_display_transform((float)(320.5+140)/1560,(float)200.5/720,0,0,2);
    assert(fabs(pending_x-160.25)<.01&&fabs(pending_y-100.25)<.01&&world_zoom==2);
    stronghold_display_transform(.5f,.5f,10.f/1560,20.f/720,1);
    assert(fabs(pending_x-155.25)<.01&&fabs(pending_y-90.25)<.01);
    /* Tapping the zoom value returns to 1:1 around the view centre. */
    world_zoom=2;unsnapped_zoom=2;pending_x=pending_y=0;
    stronghold_display_zoom_reset();
    assert(world_zoom==1&&unsnapped_zoom==1&&fabs(pending_x+320)<.01&&fabs(pending_y+180)<.01);
    pending_x=pending_y=0;stronghold_display_zoom_reset();assert(!pending_x&&!pending_y);
    /* A selected construction button is brighter in place; its surroundings are not. */
    {
        int p=2560;frame_ready=1;published_ui_scale=1;published_present_scale=1;
        for(int y=500;y<560;y++)for(int x=600;x<680;x++){
            size_t at=(size_t)y*1280+x;ui[0].pixels[at]=0x4208;ui[0].coverage[at]=255;
        }
        stronghold_display_highlight(610,510,40,30);
        const uint16_t *q=(const uint16_t *)stronghold_display_compose(NULL,1280,720,&p,16);
        assert(q[525*1280+630]!=0x4208&&(q[525*1280+630]>>11)>(0x4208>>11));
        assert(q[505*1280+605]==0x4208);
        stronghold_display_highlight(0,0,0,0);frame_ready=1;
        q=(const uint16_t *)stronghold_display_compose(NULL,1280,720,&p,16);
        assert(q[525*1280+630]==0x4208);
    }
    stronghold_display_set_image_filter(7,NAN);{float s;assert(stronghold_display_image_filter(&s)==2&&s==0);}
    stronghold_display_set_image_filter(1,3);{float s;assert(stronghold_display_image_filter(&s)==1&&s==1);}
    stronghold_display_set_ui_scale(NAN);assert(ui_scale==1);
    stronghold_display_set_ui_scale(5);assert(ui_scale==2);
    stronghold_display_set_ui_scale(-5);assert(ui_scale==.75f);
    stronghold_display_set_render_scale(.1f);assert(requested_render_scale==.5f);
    stronghold_display_set_render_scale(.75f);assert(requested_render_scale==.75f);
    stronghold_display_set_render_scale(NAN);assert(requested_render_scale==.75f);
    stronghold_display_set_render_scale(4);assert(requested_render_scale==1);
    original_pixels=0x11000000;origin_x=origin_y=0;
    source_width=published_source_width=1280;source_height=published_source_height=720;
    for(int y=0;y<720;y++)for(int x=0;x<1280;x++)MEM16(original_pixels+(uint32_t)y*MAP_PITCH+(uint32_t)x*2)=test_world(x,y);
    for(int percent=50;percent<=100;percent+=25){
        render_scale=(float)percent/100;capture_world();
        published_raster_width=raster_width;published_raster_height=raster_height;
        published_present_scale=published_ui_scale=1;frame_ready=1;
        int p=2560;const uint16_t *q=(const uint16_t *)stronghold_display_compose(NULL,1280,720,&p,16);
        assert(q[360*1280+640]==0x07e0&&q[680*1280+500]==0xffe0); /* UI is unchanged. */
        int x=101,y=103,sx=(x*raster_width/1280)*1280/raster_width,sy=(y*raster_height/720)*720/raster_height;
        assert(q[y*1280+x]==test_world(sx,sy));
        int px,py;assert(stronghold_display_world_point((x+.5f+140)/1560,(y+.5f)/720,&px,&py)==1&&px==x&&py==y);
    }
    world_zoom=1;unsnapped_zoom=1;pending_x=pending_y=0;stronghold_display_zoom_session();
    for(int i=0;i<5;i++){stronghold_display_transform(.5f,.5f,0,0,1.01f);assert(world_zoom==1);}
    stronghold_display_transform(.5f,.5f,0,0,1.01f);
    /* Slow pinches leave the detent continuously from 1:1, not by its width. */
    assert(world_zoom>1&&world_zoom<1.02f&&snap_state==2);
    for(int i=0;i<3;i++)stronghold_display_transform(.5f,.5f,0,0,1/1.01f);
    assert(world_zoom<1&&world_zoom>.97f); /* Spent: no second snap in this gesture. */
    stronghold_display_zoom_session();
    world_zoom=.985f;unsnapped_zoom=.985;pending_x=pending_y=0;
    stronghold_display_transform(.5f,.5f,0,0,1.02f);assert(world_zoom==1&&snap_state==1);
    stronghold_display_transform(.5f,.5f,.01f,0,1);assert(world_zoom==1);
    world_zoom=1.5f;unsnapped_zoom=1.5;pending_x=pending_y=0;stronghold_display_zoom_session();
    stronghold_display_transform(.5f,.5f,0,0,1/1.45f);assert(world_zoom==1); /* entering snaps */
    stronghold_display_transform(.5f,.5f,0,0,1/1.2f);assert(world_zoom<1&&snap_state==2);
    /* Store provenance, not RGB, distinguishes opaque black/cyan/key from holes. */
    current_group=2;ui[2].guest=0x11000000;ui_open=1;origin_x=origin_y=0;
    MEM32(GAME_SCREEN+0xd4)=0x13000000;MEM32(GAME_SCREEN+0x40)=2560;
    hud_pixels=0x13000000;hud_pitch=2560;
    for(int x=0;x<3;x++){
        uint16_t color=(uint16_t[]){0,0x07ff,0xf81f}[x];
        ui[2].capture[x]=0;MEM16(ui[2].guest+(uint32_t)x*2)=color;
        stronghold_display_written(ui[2].guest+(uint32_t)x*2,2);
        assert(ui[2].capture[x]==255&&composite_pixel(color,0xffff,255,color_key)==color);
    }
    ui[2].capture[3]=0;MEM16(0x13000006)=0x1234;
    stronghold_display_written(0x13000006,2);
    assert(MEM16(ui[2].guest+6)==0x1234&&ui[2].capture[3]==255); /* mode0 background/font */
    stronghold_display_clear_scope(1);MEM16(0x13000006)=color_key;
    stronghold_display_written(0x13000006,2);
    assert(MEM16(ui[2].guest+6)==color_key&&ui[2].capture[3]==0);
    stronghold_display_clear_scope(0);stronghold_display_written(0x13000006,2);
    assert(ui[2].capture[3]==255); /* An ordinary opaque write of the same RGB stays visible. */
    ui[2].capture[4]=0;stronghold_display_blended(ui[2].guest+8,2,128);
    assert(ui[2].capture[4]==127);
    stronghold_display_blended(ui[2].guest+8,2,128);assert(ui[2].capture[4]==191);
    assert(composite_pixel(0xf81f,0xffff,0,0xf81f)==0xffff);
    assert(composite_pixel(0,0xffff,255,0xf81f)==0);
    clear_batch_selfcheck();
    bulk_rows_selfcheck();
    ui_open=0;ui[2].guest=0;
    world_zoom=.5f;pending_x=pending_y=0;camera_known=0;
    MEM32(CAMERA_MAP+0x78)=1000;MEM32(CAMERA_MAP+0x7c)=1000;MEM32(CAMERA_MAP+0x90)=0;
    stronghold_display_world_begin(CAMERA_MAP);
    assert(MEM32(CAMERA_MAP+0x90)==1&&MEM32(CAMERA_MAP+0x78)==840);
    assert(source_width==1280&&source_height==720);
    stronghold_display_viewport(CAMERA_MAP);
    assert(MEM32(CAMERA_MAP+0x18b734)==720&&MEM32(CAMERA_MAP+0x8c)==164);
    stronghold_display_world_render(CAMERA_MAP);assert(MEM32(CAMERA_MAP+0x8c)==188);
    /* Report the completed frame's total zoom, not its presenter residual,
     * requested quality, HUD scale or unsnapped pinch accumulator. */
    assert(stronghold_display_world_zoom()==1);
    for(int i=0;i<SH_UI_LAYERS;i++)ui[i].used=0;
    MEM32(GAME_SCREEN+0x40)=0;ui_open=1;
    stronghold_display_set_ui_scale(2);stronghold_display_set_render_scale(.75f);
    unsnapped_zoom=1.02;
    stronghold_display_ui_end();
    assert(stronghold_display_world_zoom()==.5f&&published_present_scale==1);
    MEM32(CAMERA_MAP+0x90)=0;stronghold_display_world_begin(CAMERA_MAP);
    ui_open=1;stronghold_display_ui_end();assert(stronghold_display_world_zoom()==1);
    world_zoom=.985f;unsnapped_zoom=.985;pending_x=pending_y=0;snap_state=0;
    stronghold_display_transform(.5f,.5f,0,0,1.02f);
    assert(world_zoom==1&&unsnapped_zoom!=1);
    stronghold_display_world_begin(CAMERA_MAP);ui_open=1;stronghold_display_ui_end();
    assert(stronghold_display_world_zoom()==1);
    /* Sub-step motion must accumulate across frames and reverse without drift. */
    for(int stepped=0;stepped<2;stepped++) {
        stronghold_display_smooth_camera(!stepped);
        camera_known=0;world_zoom=unsnapped_zoom=1;
        MEM32(CAMERA_MAP+0x78)=1024;MEM32(CAMERA_MAP+0x7c)=1024;MEM32(CAMERA_MAP+0x90)=0;
        for(int i=0;i<40;i++) {
            pending_x=1;pending_y=.5;
            stronghold_display_world_begin(CAMERA_MAP);
            assert(MEM32(CAMERA_MAP+0x78)==(uint32_t)(1024+(stepped?llround((i+1)/32.)*32:i+1)));
            assert(MEM32(CAMERA_MAP+0x7c)==(uint32_t)(1024+(stepped?llround((i+1)/32.)*16:llround((i+1)/2.))));
            camera_last_x=MEM32(CAMERA_MAP+0x78);camera_last_y=MEM32(CAMERA_MAP+0x7c);
            if(camera_last_x!=camera_coordinate(camera_fraction_x,32))camera_fraction_x=camera_last_x;
            if(camera_last_y!=camera_coordinate(camera_fraction_y,16))camera_fraction_y=camera_last_y;
        }
        for(int i=0;i<40;i++) {
            pending_x=-1;pending_y=-.5;stronghold_display_world_begin(CAMERA_MAP);
            camera_last_x=MEM32(CAMERA_MAP+0x78);camera_last_y=MEM32(CAMERA_MAP+0x7c);
        }
        assert(MEM32(CAMERA_MAP+0x78)==1024&&MEM32(CAMERA_MAP+0x7c)==1024);
    }
    stronghold_display_smooth_camera(1);
    puts("camera: smooth/stepped accumulation and reverse pan PASS");
    frame_active=frame_ready=published_active=0;
    assert(fabsf(stronghold_display_ui_dy(.1f)-72)<.001f);
    puts("independent HUD: all anchors, inverse hits/letterbox, exposed world, world-only zoom, pinch anchor/isometric pan PASS");
}
int main(void)
{
    assert(stronghold_display_world_zoom()==1);
    size_t pagesize=(size_t)sysconf(_SC_PAGESIZE);
    void *page = mmap((void *)(VOID_GUEST_BASE + (GAME_SCREEN & ~(uint32_t)(pagesize-1))), pagesize,
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(page != MAP_FAILED);
    assert(mmap((void *)(VOID_GUEST_BASE + 0x10000000u), 0x400000,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    assert(mmap((void *)(VOID_GUEST_BASE + 0x11000000u),0x9000000,
                PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
    int w = 0, h = 0;
    stronghold_display_configure(0, 0); assert(!stronghold_display_requested(&w, &h));
    stronghold_display_configure(640, 480); assert(!stronghold_display_requested(&w, &h));
    stronghold_display_configure(4057, 1440); assert(!stronghold_display_requested(&w, &h));
    stronghold_display_configure(2944,1840); assert(stronghold_display_requested(&w,&h)&&w==2944&&h==1840);
    assert(stronghold_display_dimensions(GAME_SCREEN));
    assert(MEM32(GAME_SCREEN+0x18)==2944&&MEM32(GAME_SCREEN+0x1c)==1840);
    stronghold_display_configure(800, 1440); assert(!stronghold_display_requested(&w, &h));
    stronghold_display_configure(1559, 720);
    assert(stronghold_display_requested(&w, &h) && w == 1560 && h == 720);
    MEM32(GAME_SCREEN + 0x58) = 0x12345678;
    assert(!stronghold_display_dimensions(GAME_SCREEN + 4));
    assert(stronghold_display_dimensions(GAME_SCREEN));
    assert(MEM32(GAME_SCREEN + 0x18) == 1560 && MEM32(GAME_SCREEN + 0x1c) == 720);
    assert(MEM32(GAME_SCREEN + 0x5c) == 3 && MEM32(GAME_SCREEN + 0x58) == 0x12345678);
    stronghold_display_configure(1280, 600);
    assert(stronghold_display_dimensions(GAME_SCREEN) && MEM32(GAME_SCREEN + 0x1c) == 600);
    const uint32_t map = 0x10000000, renderer = 0x10200000, stack = 0x103ff000;
    for (int zoom = 0; zoom < 2; ++zoom) {
        MEM32(map + 0x90) = (uint32_t)zoom;
        MEM32(map + 0x18b730) = 1280;
        MEM32(map + 0x18b734) = 472;
        stronghold_display_viewport(map);
        assert(MEM32(map + 0x88) == (uint32_t)(45 * (zoom + 1)));
        assert(MEM32(map + 0x8c) == (uint32_t)(63 * (zoom + 1)));
    }
    MEM32(renderer + 0x16c20c) = 240; MEM32(renderer + 0x16c210) = 128;
    MEM32(stack) = 0; MEM32(stack + 4) = 0; MEM32(stack + 8) = 592;
    assert(!stronghold_display_hud_edge(renderer, stack, 0));
    assert(MEM32(stack + 4) == 0 && MEM32(stack + 8) == 472);
    stronghold_display_configure(1560, 720);
    assert(stronghold_display_half_width() == 380);
    MEM32(map + 0x90) = 0; MEM32(map + 0x18b730) = 1560; MEM32(map + 0x18b734) = 592;
    stronghold_display_viewport(map);
    assert(MEM32(map + 0x88) == 54 && MEM32(map + 0x8c) == 78);
    MEM32(renderer + 0x16c20c) = 880;
    assert(stronghold_display_hud_edge(renderer, stack, 0));
    assert((int32_t)MEM32(stack + 4) == -500 && MEM32(stack + 8) == 592);
    MEM32(stack + 4) = 1040;
    assert(stronghold_display_hud_edge(renderer, stack, 1));
    assert(MEM32(stack + 4) == 1180);
    MEM32(stack + 4) = 1024;
    assert(stronghold_display_hud_edge(renderer, stack, 1));
    assert(MEM32(stack + 4) == 1164);
    uint32_t saved[4];
    for (unsigned i = 0; i < 4; ++i) MEM32(renderer + 0x16c854 + i * 4) = 10 + i;
    stronghold_display_hud_clip(renderer, saved, 0);
    assert(MEM32(renderer + 0x16c854) == 0 && MEM32(renderer + 0x16c858) == 0);
    assert(MEM32(renderer + 0x16c85c) == 1560 && MEM32(renderer + 0x16c860) == 720);
    stronghold_display_hud_clip(renderer, saved, 1);
    for (unsigned i = 0; i < 4; ++i) assert(MEM32(renderer + 0x16c854 + i * 4) == 10 + i);
    stronghold_display_clear(stack);
    assert(MEM32(stack + 8) == 1559 && MEM32(stack + 12) == 719);
    assert(stronghold_display_edge_name(0x5a3374 + GAME_RESOURCE_DELTA) == 0x5a3334 + GAME_RESOURCE_DELTA);
    stronghold_display_configure(1280, 720);
    assert(stronghold_display_edge_name(0x5a3374 + GAME_RESOURCE_DELTA) == 0x5a3374 + GAME_RESOURCE_DELTA);
    MEM32(renderer + 0x16c20c) = 240; MEM32(stack + 4) = 1040;
    assert(!stronghold_display_hud_edge(renderer, stack, 1));
    assert(MEM32(stack + 4) == 1040 && MEM32(stack + 8) == 592);
    stronghold_display_configure(0, 0);
    assert(stronghold_display_width(1920) == 1920 && stronghold_display_height(1080) == 1080);
    assert(stronghold_display_half_width() == 240);
    composition_selfcheck();
    resource_selfcheck();
    notice_selfcheck();
    gpu_frame_selfcheck();
    overlay_geometry_selfcheck();
    /* The powers sidebar has its own scale and inverse, without shrinking the HUD. */
    if(UI_POWERS){
        stronghold_display_configure(1280,720);stronghold_display_surface_size(1280,720);
        published_width=1280;published_height=720;published_active=1;
        for(int i=0;i<SH_UI_LAYERS;i++){ui[i].shown=(stronghold_rect){0};published_used[i]=0;}
        ui[1].shown=(stronghold_rect){1225,56,1280,376};published_used[1]=ui[1].used=1;
        ui_scale=1.2f;published_hud_opaque=top_overlay_used=0;
        for(int percent=75;percent<=300;percent+=25){
            stronghold_display_powers_scale(percent/100.f);publish_geometry();
            assert(fabsf(published_ui_scale-1.2f)<.001f);
            float s=layer_scale(1);assert(s<=720.f/320+.001f&&s<=percent/100.f+.001f);
            float x,y,view_scale;int gx,gy;
            assert(stronghold_display_layer_view(1,1250.5f,100.5f,&x,&y,&view_scale));
            assert(fabsf(s-view_scale)<.001f&&x>=0&&x<1280&&y>=0&&y<720);
            assert(stronghold_display_ui_point(1,x/1280,y/720,&gx,&gy)&&gx==1250&&gy==100);
            frame_ready=1;stronghold_frame f;assert(stronghold_display_frame(&f,1280,720,16));
            assert(f.parts==1&&f.part[0].layer==1&&fabsf(f.part[0].scale-s)<.001f);
        }
        stronghold_display_powers_scale(NAN);assert(powers_scale==3);
        powers_scale=published_powers_scale=0;
        puts("Extreme powers: independent75..300%, fit, GPU part scale and inverse hits PASS");
    }
    assert(stronghold_notice_margin(1600,720,2400,1080,.05f,.75f)==123);
    puts("custom display: bounds/alignment, viewport/zoom, HUD crop/bottom, clear, baseline/fallback PASS");
    return 0;
}
#endif
