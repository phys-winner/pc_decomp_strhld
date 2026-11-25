/* The engine's RGB565 targets and the native GLES present pass. The screen
 * hooks bind the original calls at build time, before any COM dispatch. */
#include "host.h"
#include "input.h"
#include "edition.h"
#include "display.h"
#include "renderer.h"
#include <assert.h>

#define RETURN(value, nargs) do { g_eax = (value); g_esp += 4u + 4u*(nargs); return; } while (0)

typedef struct render_surface {
    uint32_t token, pixels, allocation, caps;
    uint8_t *px;
    int w, h, pitch, bpp, refs, locks, dcs, primary, key;
    uint32_t key_lo, key_hi;
    struct render_surface *back, *next;
} render_surface;

static render_surface *primary, *surfaces;
static int mode_w = 1024, mode_h = 768;
static uint32_t screen_token;

static int first_run_mode(uint32_t *index)
{
    if (*index != UINT32_MAX) return 0;
    *index = 3;
    return 1;
}

static render_surface *surface(uint32_t token)
{
    for (render_surface *s = surfaces; s; s = s->next)
        if (s->token == token) return s;
    return NULL;
}

static void mark(const render_surface *s)
{
    if (s == primary) stronghold_renderer_present_pixels(s->px, s->w, s->h, s->pitch, s->bpp);
}

/* Surface guest VAs are opaque HDC tokens; lookup never truncates a pointer. */
int stronghold_renderer_dc_pixels(uint32_t dc,uint32_t *dst,int *w,int *h,int *pitch,int *bpp)
{
    for(render_surface *s=surfaces;s;s=s->next)if(s->token==dc&&s->dcs){
        *dst=s->pixels;*w=s->w;*h=s->h;*pitch=s->pitch;*bpp=s->bpp;return 1;
    }
    return 0;
}

/* The layered HUD redirects both the cached raster pointer and the buffer
 * used by the text and video paths. */
uint32_t stronghold_renderer_redirect(uint32_t token,uint32_t pixels)
{
    render_surface *s;
    for(s=surfaces;s;s=s->next)if(s->token==token)break;
    if(!s||s->primary||s->bpp!=16||s->locks||s->dcs||
       !void_vm_writable(pixels,(uint32_t)((size_t)s->pitch*s->h)))return 0;
    uint32_t old=s->pixels;s->pixels=pixels;s->px=GPTR(pixels);return old;
}

static uint32_t pix(const render_surface *s,int x,int y)
{
    const uint8_t *p=s->px+(size_t)y*s->pitch+(size_t)x*(s->bpp/8);
    return s->bpp==16 ? *(const uint16_t *)p : s->bpp==8 ? *p : s->bpp==32 ? *(const uint32_t *)p : p[0]|p[1]<<8|p[2]<<16;
}
static void putpix(render_surface *s,int x,int y,uint32_t c)
{
    uint8_t *p=s->px+(size_t)y*s->pitch+(size_t)x*(s->bpp/8);
    if(s->bpp==16) *(uint16_t *)p=(uint16_t)c;
    else if(s->bpp==32) *(uint32_t *)p=c;
    else if(s->bpp==8) *p=(uint8_t)c;
    else { p[0]=(uint8_t)c; p[1]=(uint8_t)(c>>8); p[2]=(uint8_t)(c>>16); }
    if(s->bpp==16&&stronghold_display_capture_active)
        stronghold_display_written(s->pixels+(uint32_t)((size_t)y*s->pitch+(size_t)x*2),2);
}
typedef struct { int l,t,r,b; } render_rect;
static render_rect rect(uint32_t p,const render_surface *s)
{
    if(p) return (render_rect){(int)MEM32(p),(int)MEM32(p+4),(int)MEM32(p+8),(int)MEM32(p+12)};
    return (render_rect){0,0,s->w,s->h};
}
static void fill(render_surface *s,render_rect r,uint32_t c)
{
    if(r.l<0)r.l=0; if(r.t<0)r.t=0;
    if(r.r>s->w)r.r=s->w; if(r.b>s->h)r.b=s->h;
    if(r.l>=r.r || r.t>=r.b)return;
    for(int y=r.t;y<r.b;y++) {
        if(s->bpp==16) { uint16_t *p=(uint16_t *)(s->px+(size_t)y*s->pitch)+r.l;
            for(int x=r.l;x<r.r;x++)*p++=(uint16_t)c;
            if(stronghold_display_capture_active){
                if(s->pitch<=8112)stronghold_display_written(s->pixels+(uint32_t)((size_t)y*s->pitch+(size_t)r.l*2),(unsigned)(r.r-r.l)*2);
                else for(int x=r.l;x<r.r;x++)stronghold_display_written(s->pixels+(uint32_t)((size_t)y*s->pitch+(size_t)x*2),2);
            }
        }
        else for(int x=r.l;x<r.r;x++)putpix(s,x,y,c);
    }
}
/* Equal-size unkeyed copies use row memcpy; scaled/keyed copies retain the
 * unclipped origin so clipping cannot shift the texture coordinates. */
static uint32_t blit(render_surface *d,render_rect dr,render_surface *s,render_rect sr,int key,uint32_t lo,uint32_t hi)
{
    int dw=dr.r-dr.l, dh=dr.b-dr.t, sw=sr.r-sr.l, sh=sr.b-sr.t;
    if(dw<=0||dh<=0||sw<=0||sh<=0||d->bpp!=s->bpp)return E_INVALIDARG_;
    render_surface src=*s; uint8_t *copy=NULL;
    if(s==d) { copy=malloc((size_t)s->pitch*s->h); if(!copy)return E_OUTOFMEMORY_; memcpy(copy,s->px,(size_t)s->pitch*s->h); src.px=copy; }
    int x0=dr.l<0?-dr.l:0, y0=dr.t<0?-dr.t:0;
    int x1=dw, y1=dh;
    if(dr.l+x1>d->w)x1=d->w-dr.l; if(dr.t+y1>d->h)y1=d->h-dr.t;
    if(dw==sw&&dh==sh&&!key) {
        if(sr.l+x0<0)x0=-sr.l; if(sr.t+y0<0)y0=-sr.t;
        if(sr.l+x1>s->w)x1=s->w-sr.l; if(sr.t+y1>s->h)y1=s->h-sr.t;
        for(int y=y0;y<y1&&x1>x0;y++){
            memcpy(d->px+(size_t)(dr.t+y)*d->pitch+(size_t)(dr.l+x0)*(d->bpp/8),
                src.px+(size_t)(sr.t+y)*s->pitch+(size_t)(sr.l+x0)*(s->bpp/8),(size_t)(x1-x0)*(d->bpp/8));
            if(d->bpp==16&&stronghold_display_capture_active){
                if(d->pitch<=8112)stronghold_display_written(d->pixels+(uint32_t)((size_t)(dr.t+y)*d->pitch+(size_t)(dr.l+x0)*2),(unsigned)(x1-x0)*2);
                else for(int x=x0;x<x1;x++)stronghold_display_written(d->pixels+(uint32_t)((size_t)(dr.t+y)*d->pitch+(size_t)(dr.l+x)*2),2);
            }
        }
    } else for(int y=y0;y<y1;y++) {
        int sy=sr.t+(int)((int64_t)y*sh/dh); if(sy<0||sy>=s->h)continue;
        for(int x=x0;x<x1;x++) { int sx=sr.l+(int)((int64_t)x*sw/dw); if(sx<0||sx>=s->w)continue;
            uint32_t c=pix(&src,sx,sy); if(!key||c<lo||c>hi)putpix(d,dr.l+x,dr.t+y,c); }
    }
    free(copy); return 0;
}
static void pixel_format(uint32_t p,int bpp)
{
    memset(GPTR(p),0,32); MEM32(p)=32; MEM32(p+4)=bpp==8?0x60:0x40; MEM32(p+12)=bpp;
    MEM32(p+16)=bpp==16?0xF800:bpp==8?0:0xFF0000;
    MEM32(p+20)=bpp==16?0x07E0:bpp==8?0:0x00FF00;
    MEM32(p+24)=bpp==16?0x001F:bpp==8?0:0x0000FF;
}
static int description(uint32_t p,const render_surface *s)
{
    if(!p||!void_vm_readable(p,4))return 0;
    uint32_t n=MEM32(p); if((n!=108&&n!=124)||!void_vm_writable(p,n))return 0;
    memset(GPTR(p+4),0,n-4); MEM32(p+4)=0x180F; MEM32(p+8)=s->h; MEM32(p+12)=s->w;
    MEM32(p+16)=s->pitch; MEM32(p+36)=s->pixels; MEM32(p+104)=s->caps;
    if(s->key) { MEM32(p+4)|=0x10000; MEM32(p+64)=s->key_lo; MEM32(p+68)=s->key_hi; }
    pixel_format(p+72,s->bpp); return 1;
}
static uint32_t release_surface(render_surface *s)
{
    if (!s) return 0;
    if (--s->refs) return (uint32_t)s->refs;
    render_surface **link = &surfaces;
    while (*link != s) link = &(*link)->next;
    *link = s->next;
    if (primary == s) primary = NULL;
    release_surface(s->back);
    if (s->allocation) void_gfree(s->allocation);
    void_gfree(s->token);
    free(s);
    return 0;
}

static render_surface *new_surface(int w, int h, int bpp, uint32_t caps)
{
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || bpp != 16) return NULL;
    render_surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    s->bpp = bpp;
    s->pitch = (w * 2 + 3) & ~3;
    s->caps = caps;
    s->refs = 1;
    s->allocation = s->pixels = void_galloc((uint32_t)s->pitch * h);
    if (s->pixels) s->token = void_galloc(4);
    if (!s->token) {
        if (s->allocation) void_gfree(s->allocation);
        free(s);
        return NULL;
    }
    MEM32(s->token) = 0;
    s->px = GPTR(s->pixels);
    memset(s->px, 0, (size_t)s->pitch * h);
    s->next = surfaces;
    surfaces = s;
    return s;
}

void stronghold_render_open(void)
{
    uint32_t out = ARG(1);
    if (!out || !void_vm_writable(out, 4)) RETURN(E_POINTER_, 3);
    int width = 1280, height = 720, custom = stronghold_display_requested(&width, &height);
    if (GAME_SCREEN && void_vm_writable(GAME_SCREEN, 0x60) &&
        (custom || first_run_mode(GPTR(GAME_SCREEN + 0x5c)))) {
        MEM32(GAME_SCREEN + 0x5c) = 3;
        void_call_guest_this(GAME_MODE_CALC, GAME_SCREEN, 0, NULL);
        assert(MEM32(GAME_SCREEN + 0x18) == (uint32_t)width &&
               MEM32(GAME_SCREEN + 0x1c) == (uint32_t)height && MEM32(GAME_SCREEN + 0x40) == (uint32_t)width * 2);
    }
    if (!screen_token) {
        screen_token = void_galloc(4);
        if (screen_token) MEM32(screen_token) = 0;
    }
    MEM32(out) = screen_token;
    RETURN(screen_token ? 0 : E_OUTOFMEMORY_, 3);
}

void stronghold_render_close(void)
{
    if (screen_token && ARG(0) == screen_token) {
        void_gfree(screen_token);
        screen_token = 0;
    }
    RETURN(0, 1);
}

void stronghold_render_configure(void) { RETURN(0, 3); }

void stronghold_render_set_mode(void)
{
    int w = (int)ARG(1), h = (int)ARG(2);
    if (w < 320 || h < 200 || w > 8192 || h > 8192 || ARG(3) != 16) RETURN(E_INVALIDARG_, 4);
    mode_w = w;
    mode_h = h;
    void_input_set_screen_size(w, h);
    void_log("[renderer] native mode %dx%dx16", w, h);
    RETURN(0, 4);
}

void stronghold_render_capabilities(void)
{
    uint32_t p = ARG(1);
    if (!p || !void_vm_readable(p, 4) || MEM32(p) != 380 || !void_vm_writable(p, 380))
        RETURN(E_INVALIDARG_, 3);
    memset(GPTR(p + 4), 0, 376);
    MEM32(p + 4) = 0x04404240;
    MEM32(p + 12) = 0x200;
    MEM32(p + 60) = 256u << 20;
    MEM32(p + 64) = 192u << 20;
    MEM32(p + 104) = 0xC858;
    RETURN(0, 3);
}

void stronghold_render_modes(void)
{
    static const int modes[][2] = {{800,600},{1024,768},{1280,720},{1280,800},{1280,960},
                                  {1366,768},{1600,900},{1920,1080},{2560,1600}};
    uint32_t cb = ARG(4), ctx = ARG(3);
    if (!cb) RETURN(E_INVALIDARG_, 5);
    uint32_t desc = void_galloc(108);
    if (!desc) RETURN(E_OUTOFMEMORY_, 5);
    int width = 0, height = 0, custom = stronghold_display_requested(&width, &height);
    const unsigned standard = sizeof modes / sizeof modes[0];
    for (unsigned i = 0; i < standard + custom; ++i) {
        int w = i < standard ? modes[i][0] : width;
        int h = i < standard ? modes[i][1] : height;
        render_surface s = {.w = w, .h = h, .pitch = w * 2, .bpp = 16};
        MEM32(desc) = 108;
        description(desc, &s);
        MEM32(desc + 4) |= 0x40000;
        MEM32(desc + 24) = 60;
        const uint32_t args[] = {desc, ctx};
        if (!void_call_guest(cb, 2, args, 1)) break;
    }
    void_gfree(desc);
    RETURN(0, 5);
}

void stronghold_render_allocate(void)
{
    uint32_t desc = ARG(1), out = ARG(2);
    if (!desc || !void_vm_readable(desc, 108) || !out || !void_vm_writable(out, 4)) RETURN(E_INVALIDARG_, 4);
    MEM32(out) = 0;
    uint32_t flags = MEM32(desc + 4), caps = (flags & 1) ? MEM32(desc + 104) : 0;
    int front = (caps & 0x200) != 0;
    int w = front ? mode_w : (int)MEM32(desc + 12);
    int h = front ? mode_h : (int)MEM32(desc + 8);
    int bpp = (!front && (flags & 0x1000)) ? (int)MEM32(desc + 84) : 16;
    render_surface *s = new_surface(w, h, bpp, caps | 0x800);
    if (!s) RETURN(E_OUTOFMEMORY_, 4);
    if ((flags & 0x800) && MEM32(desc + 36)) {
        int pitch = (flags & 8) ? (int)MEM32(desc + 16) : s->pitch;
        uint32_t pixels = MEM32(desc + 36);
        if (pitch < s->pitch || (uint64_t)pitch * h > UINT32_MAX || !void_vm_writable(pixels, (uint32_t)pitch * h)) {
            release_surface(s);
            RETURN(E_INVALIDARG_, 4);
        }
        void_gfree(s->allocation);
        s->allocation = 0;
        s->pixels = pixels;
        s->px = GPTR(pixels);
        s->pitch = pitch;
    }
    if (flags & 0x10000) {
        s->key = 1;
        s->key_lo = MEM32(desc + 64);
        s->key_hi = MEM32(desc + 68);
    }
    if (front) {
        if ((flags & 0x20) && MEM32(desc + 20)) {
            s->back = new_surface(w, h, bpp, 0x81C);
            if (!s->back) {
                release_surface(s);
                RETURN(E_OUTOFMEMORY_, 4);
            }
        }
        s->primary = 1;
        s->caps |= 0x8020;
        primary = s;
    }
    MEM32(out) = s->token;
    RETURN(0, 4);
}

void stronghold_render_release(void) { RETURN(release_surface(surface(ARG(0))), 1); }

void stronghold_render_back_buffer(void)
{
    uint32_t out = ARG(2);
    render_surface *s = surface(ARG(0));
    if (!out || !void_vm_writable(out, 4)) RETURN(E_POINTER_, 3);
    MEM32(out) = 0;
    if (!s || !s->back) RETURN(E_INVALIDARG_, 3);
    MEM32(out) = s->back->token;
    ++s->back->refs;
    RETURN(0, 3);
}

void stronghold_render_describe(void)
{
    render_surface *s = surface(ARG(0));
    RETURN(s && description(ARG(1), s) ? 0 : E_INVALIDARG_, 2);
}

void stronghold_render_format(void)
{
    render_surface *s = surface(ARG(0));
    uint32_t out = ARG(1);
    if (!s || !out || !void_vm_writable(out, 32)) RETURN(E_INVALIDARG_, 2);
    pixel_format(out, s->bpp);
    RETURN(0, 2);
}

void stronghold_render_lock(void)
{
    render_surface *s = surface(ARG(0));
    if (!s) RETURN(E_INVALIDARG_, 5);
    render_rect r = rect(ARG(1), s);
    uint32_t desc = ARG(2);
    if (r.l < 0 || r.t < 0 || r.r > s->w || r.b > s->h || r.l >= r.r || r.t >= r.b || !description(desc, s))
        RETURN(E_INVALIDARG_, 5);
    MEM32(desc + 36) = s->pixels + (uint32_t)r.t * s->pitch + (uint32_t)r.l * (s->bpp / 8);
    ++s->locks;
    RETURN(0, 5);
}

void stronghold_render_unlock(void)
{
    render_surface *s = surface(ARG(0));
    if (!s || !s->locks) RETURN(0x887602ACu, 2);
    --s->locks;
    mark(s);
    RETURN(0, 2);
}

void stronghold_render_restore(void) { RETURN(0, 1); }

void stronghold_render_copy(void)
{
    render_surface *dst = surface(ARG(0));
    if (!dst) RETURN(E_INVALIDARG_, 6);
    render_rect dr = rect(ARG(1), dst);
    uint32_t flags = ARG(4), effect = ARG(5), result = 0;
    if (flags & 0x400) {
        if (!effect || !void_vm_readable(effect, 84)) RETURN(E_INVALIDARG_, 6);
        fill(dst, dr, MEM32(effect + 80));
    } else {
        render_surface *src = surface(ARG(2));
        if (!src) RETURN(E_INVALIDARG_, 6);
        int keyed = (flags & 0x8000) && src->key;
        uint32_t lo = src->key_lo, hi = src->key_hi;
        if (flags & 0x10000) {
            if (!effect || !void_vm_readable(effect, 100)) RETURN(E_INVALIDARG_, 6);
            keyed = 1;
            lo = MEM32(effect + 92);
            hi = MEM32(effect + 96);
        }
        result = blit(dst, dr, src, rect(ARG(3), src), keyed, lo, hi < lo ? lo : hi);
    }
    if (!result) mark(dst);
    RETURN(result, 6);
}

void stronghold_render_copy_at(void)
{
    render_surface *dst = surface(ARG(0)), *src = surface(ARG(3));
    if (!dst || !src) RETURN(E_INVALIDARG_, 6);
    render_rect sr = rect(ARG(4), src);
    int x = (int)ARG(1), y = (int)ARG(2);
    render_rect dr = {x, y, x + sr.r - sr.l, y + sr.b - sr.t};
    uint32_t result = blit(dst, dr, src, sr, (ARG(5) & 1) && src->key, src->key_lo, src->key_hi);
    if (!result) mark(dst);
    RETURN(result, 6);
}

void stronghold_render_swap(void)
{
    render_surface *s = surface(ARG(0));
    render_surface *b = ARG(1) ? surface(ARG(1)) : (s ? s->back : NULL);
    if (!s || !b || s->w != b->w || s->h != b->h || s->bpp != b->bpp || s->pitch != b->pitch)
        RETURN(E_INVALIDARG_, 3);
    uint8_t *px = s->px;
    uint32_t pixels = s->pixels, allocation = s->allocation;
    s->px = b->px; s->pixels = b->pixels; s->allocation = b->allocation;
    b->px = px; b->pixels = pixels; b->allocation = allocation;
    mark(s);
    RETURN(0, 3);
}

void stronghold_render_acquire_dc(void)
{
    uint32_t out = ARG(1);
    render_surface *s = surface(ARG(0));
    if (!out || !void_vm_writable(out, 4)) RETURN(E_POINTER_, 2);
    if (!s) RETURN(E_INVALIDARG_, 2);
    ++s->dcs;
    ++s->refs;
    MEM32(out) = s->token;
    RETURN(0, 2);
}

void stronghold_render_release_dc(void)
{
    render_surface *s = surface(ARG(0));
    if (!s || ARG(1) != s->token || !s->dcs) RETURN(E_INVALIDARG_, 2);
    --s->dcs;
    mark(s);
    release_surface(s);
    RETURN(0, 2);
}

int stronghold_renderer_selftest(void)
{
    uint32_t selected=UINT32_MAX;assert(first_run_mode(&selected)&&selected==3);
    selected=2;assert(!first_run_mode(&selected)&&selected==2);
    selected=10;assert(!first_run_mode(&selected)&&selected==10);
    uint16_t a[12]={1,2,3,0xCCCC,4,0,6,0xCCCC,7,8,9,0xCCCC},b[15];
    memset(b,0xAA,sizeof b);render_surface s={.px=(uint8_t *)a,.w=3,.h=3,.pitch=8,.bpp=16};
    render_surface d={.px=(uint8_t *)b,.w=4,.h=3,.pitch=10,.bpp=16};
    assert(!blit(&d,(render_rect){-1,0,2,3},&s,(render_rect){0,0,3,3},0,0,0));
    assert(b[0]==2&&b[1]==3&&b[5]==0&&b[6]==6&&b[10]==8&&b[11]==9&&b[4]==0xAAAA);
    fill(&d,(render_rect){0,0,4,3},0xF800);assert(b[0]==0xF800&&b[13]==0xF800&&b[14]==0xAAAA);
    assert(!blit(&d,(render_rect){0,0,3,3},&s,(render_rect){0,0,3,3},1,0,0));assert(b[6]==0xF800&&b[7]==6);
    assert(!blit(&s,(render_rect){1,0,3,3},&s,(render_rect){0,0,2,3},0,0,0));assert(a[1]==1&&a[2]==2&&a[3]==0xCCCC);
    assert(!blit(&d,(render_rect){0,0,4,2},&s,(render_rect){0,0,2,1},0,0,0));assert(b[0]==1&&b[1]==1&&b[2]==1&&b[3]==1);
    assert(blit(&d,(render_rect){0,0,0,2},&s,(render_rect){0,0,1,1},0,0,0)==E_INVALIDARG_);
    void_log("[selftest] renderer CPU: PASS");return 0;
}
