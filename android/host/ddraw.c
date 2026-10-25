/* Stronghold's DirectDraw 1 software surfaces. The sole embedded DD7 IID
 * (EXE 005A860C) is a capability probe; 00470220 creates the real DD1 device
 * and 00470369 selects 16-bit RGB. No host pointer is stored in guest memory. */
#include "com.h"
#include "input.h"
#include "edition.h"
#include "display.h"
#include <assert.h>
#include <time.h>

#define DD_INVALID E_INVALIDARG_
#define DD_NOTFOUND 0x887600FFu
#define DD_NOCOLORKEY 0x887600D7u
#define DD_NOCLIPPER 0x88760082u
#define DD_NOPALETTE 0x8876023Cu
#define DD_NOTLOCKED 0x887602ACu
#define DD_UNSUPPORTED 0x80004001u
#define A(i) ARG(i)
#define RET(r,n) do { g_eax = (r); g_esp += 4u + 4u*(n); return; } while (0)

typedef struct dd_surface {
    void_com_object com;
    uint32_t pixels, caps, palette, clipper;
    uint8_t *px;
    int w, h, pitch, bpp, owned, locks, dcs, primary, key;
    uint32_t key_lo, key_hi;
    struct dd_surface *back, *front;
    struct dd_surface *next;
} dd_surface;
typedef struct { void_com_object com; int version; } dd_device;
typedef struct { void_com_object com; uint32_t hwnd; } dd_clipper;
typedef struct { void_com_object com; uint32_t entries[256]; } dd_palette;
static void_com_class dd1_class, dd2_class, dd7_class, surface_class, clipper_class, palette_class;
static dd_surface *primary;
static dd_surface *surfaces;
static int mode_w = 1024, mode_h = 768, mode_bpp = 16;
static uint32_t last_dd;

static int first_run_mode(uint32_t *index)
{
    if(*index!=UINT32_MAX)return 0;
    *index=3;return 1;
}

/* Surface guest VAs are opaque HDC tokens; lookup never truncates a pointer. */
int void_ddraw_dc_pixels(uint32_t dc,uint32_t *dst,int *w,int *h,int *pitch,int *bpp)
{
    for(dd_surface *s=surfaces;s;s=s->next)if(s->com.guest==dc&&s->dcs){
        *dst=s->pixels;*w=s->w;*h=s->h;*pitch=s->pitch;*bpp=s->bpp;return 1;
    }
    return 0;
}

/* UI rasterization includes GetDC/Bink users of the surface, not just the
 * EXE's cached framebuffer VA. Restore before the game's present/teardown. */
uint32_t void_ddraw_redirect_pixels(uint32_t token,uint32_t pixels)
{
    dd_surface *s;
    for(s=surfaces;s;s=s->next)if(s->com.guest==token)break;
    if(!s||s->primary||s->bpp!=16||s->locks||s->dcs||
       !void_vm_writable(pixels,(uint32_t)((size_t)s->pitch*s->h)))return 0;
    uint32_t old=s->pixels;s->pixels=pixels;s->px=GPTR(pixels);return old;
}

void void_ddraw_present_pixels(const uint8_t *px, int w, int h, int pitch, int bpp);
int void_ddraw_frame(const uint8_t **px, int *w, int *h, int *pitch, int *bpp)
{
    if (!primary) return 0;
    *px=primary->px; *w=primary->w; *h=primary->h; *pitch=primary->pitch; *bpp=primary->bpp;
    return 1;
}
void void_ddraw_present(void)
{
    if (primary) void_ddraw_present_pixels(primary->px,primary->w,primary->h,primary->pitch,primary->bpp);
}
static dd_surface *surface(uint32_t p) { return (dd_surface *)void_com_host(p); }
static void mark(dd_surface *s) { if (s == primary) void_ddraw_present(); }
static uint32_t pix(const dd_surface *s,int x,int y)
{
    const uint8_t *p=s->px+(size_t)y*s->pitch+(size_t)x*(s->bpp/8);
    return s->bpp==16 ? *(const uint16_t *)p : s->bpp==8 ? *p : s->bpp==32 ? *(const uint32_t *)p : p[0]|p[1]<<8|p[2]<<16;
}
static void putpix(dd_surface *s,int x,int y,uint32_t c)
{
    uint8_t *p=s->px+(size_t)y*s->pitch+(size_t)x*(s->bpp/8);
    if(s->bpp==16) *(uint16_t *)p=(uint16_t)c;
    else if(s->bpp==32) *(uint32_t *)p=c;
    else if(s->bpp==8) *p=(uint8_t)c;
    else { p[0]=(uint8_t)c; p[1]=(uint8_t)(c>>8); p[2]=(uint8_t)(c>>16); }
    if(s->bpp==16&&stronghold_display_capture_active)
        stronghold_display_written(s->pixels+(uint32_t)((size_t)y*s->pitch+(size_t)x*2),2);
}
typedef struct { int l,t,r,b; } dd_rect;
static dd_rect rect(uint32_t p,const dd_surface *s)
{
    if(p) return (dd_rect){(int)MEM32(p),(int)MEM32(p+4),(int)MEM32(p+8),(int)MEM32(p+12)};
    return (dd_rect){0,0,s->w,s->h};
}
static void fill(dd_surface *s,dd_rect r,uint32_t c)
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
static uint32_t blit(dd_surface *d,dd_rect dr,dd_surface *s,dd_rect sr,int key,uint32_t lo,uint32_t hi)
{
    int dw=dr.r-dr.l, dh=dr.b-dr.t, sw=sr.r-sr.l, sh=sr.b-sr.t;
    if(dw<=0||dh<=0||sw<=0||sh<=0||d->bpp!=s->bpp)return DD_INVALID;
    dd_surface src=*s; uint8_t *copy=NULL;
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
static int description(uint32_t p,const dd_surface *s)
{
    if(!p||!void_vm_readable(p,4))return 0;
    uint32_t n=MEM32(p); if((n!=108&&n!=124)||!void_vm_writable(p,n))return 0;
    memset(GPTR(p+4),0,n-4); MEM32(p+4)=0x180F; MEM32(p+8)=s->h; MEM32(p+12)=s->w;
    MEM32(p+16)=s->pitch; MEM32(p+36)=s->pixels; MEM32(p+104)=s->caps;
    if(s->key) { MEM32(p+4)|=0x10000; MEM32(p+64)=s->key_lo; MEM32(p+68)=s->key_hi; }
    pixel_format(p+72,s->bpp); return 1;
}
static void surface_destroy(void_com_object *o)
{
    dd_surface *s=(dd_surface *)o;
    for(dd_surface **p=&surfaces;*p;p=&(*p)->next)if(*p==s){*p=s->next;break;}
    if(primary==s)primary=NULL;
    if(s->front)s->front->back=NULL;
    if(s->back) { s->back->front=NULL; void_com_release(&s->back->com); }
    if(s->clipper)void_com_release(void_com_host(s->clipper));
    if(s->palette)void_com_release(void_com_host(s->palette));
    if(s->owned)void_gfree(s->pixels);
    free(s);
}
static dd_surface *new_surface(int w,int h,int bpp,uint32_t caps)
{
    if(w<=0||h<=0||w>8192||h>8192||(bpp!=8&&bpp!=16&&bpp!=24&&bpp!=32))return NULL;
    dd_surface *s=calloc(1,sizeof *s); if(!s)return NULL;
    s->w=w;s->h=h;s->bpp=bpp;s->pitch=(w*(bpp/8)+3)&~3;s->caps=caps;
    s->pixels=void_galloc((uint32_t)s->pitch*h); if(!s->pixels){free(s);return NULL;}
    s->px=GPTR(s->pixels);s->owned=1;memset(s->px,0,(size_t)s->pitch*h);
    s->com.destroy=surface_destroy;void_com_wrap(&s->com,&surface_class,0);
    s->next=surfaces;surfaces=s;return s;
}
static uint32_t new_device(int version)
{
    dd_device *d=calloc(1,sizeof *d);if(!d)return 0;d->version=version;
    return void_com_wrap(&d->com,version==1?&dd1_class:version==2?&dd2_class:&dd7_class,0);
}
static int dd_version(uint32_t iid)
{
    static const uint8_t ids[][16]={
        {0x80,0xDB,0x14,0x6C,0x33,0xA7,0xCE,0x11,0xA5,0x21,0,0x20,0xAF,0x0B,0xE5,0x60},
        {0xE0,0xF3,0xA6,0xB3,0x43,0x2B,0xCF,0x11,0xA2,0xDE,0,0xAA,0,0xB9,0x33,0x56},
        {0x9A,0x50,0x59,0x9C,0xBD,0x39,0xD1,0x11,0x8C,0x4A,0,0xC0,0x4F,0xD9,0x30,0xC5},
        {0xC0,0x5E,0xE6,0x15,0x9C,0x3B,0xD2,0x11,0xB9,0x2F,0,0x60,0x97,0x97,0xEA,0x5B}};
    if(!iid||!void_vm_readable(iid,16))return 0;
    for(int i=0;i<4;i++)if(!memcmp(GPTR(iid),ids[i],16))return (int[]){1,2,4,7}[i];
    return 0;
}
static int unknown_iid(uint32_t p)
{
    static const uint8_t iid[16]={0,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46};
    return p&&void_vm_readable(p,16)&&!memcmp(GPTR(p),iid,16);
}
static void addref(void){RET(void_com_addref(void_com_host(A(0))),1);}
static void release(void){RET(void_com_release(void_com_host(A(0))),1);}
static void dd_QueryInterface(void)
{
    uint32_t out=A(2);if(!out||!void_vm_writable(out,4))RET(E_POINTER_,3);
    int v=dd_version(A(1));MEM32(out)=0;
    if(unknown_iid(A(1))) { MEM32(out)=A(0);void_com_addref(void_com_host(A(0)));RET(0,3); }
    if(v){MEM32(out)=new_device(v);RET(MEM32(out)?0:E_OUTOFMEMORY_,3);}RET(E_NOINTERFACE_,3);
}
static void sf_QueryInterface(void)
{
    uint32_t iid=A(1),out=A(2);if(!out||!void_vm_writable(out,4))RET(E_POINTER_,3);
    static const uint8_t ids[][16]={
        {0x81,0xDB,0x14,0x6C,0x33,0xA7,0xCE,0x11,0xA5,0x21,0,0x20,0xAF,0x0B,0xE5,0x60},
        {0x85,0x58,0x80,0x57,0xEC,0x6E,0xCF,0x11,0x94,0x41,0xA8,0x23,3,0xC1,0x0E,0x27},
        {0,0x4E,4,0xDA,0xB2,0x69,0xD0,0x11,0xA1,0xD5,0,0xAA,0,0xB8,0xDF,0xBB},
        {0x30,0x86,0x2B,0x0B,0x35,0xAD,0xD0,0x11,0x8E,0xA6,0,0x60,0x97,0x97,0xEA,0x5B},
        {0x80,0x5A,0x67,6,0x9B,0x3B,0xD2,0x11,0xB9,0x2F,0,0x60,0x97,0x97,0xEA,0x5B}};
    int ok=unknown_iid(iid);
    if(iid&&void_vm_readable(iid,16))for(int i=0;i<5;i++)ok|=!memcmp(GPTR(iid),ids[i],16);
    MEM32(out)=ok?A(0):0;if(ok)void_com_addref(void_com_host(A(0)));RET(ok?0:E_NOINTERFACE_,3);
}
static void base_QueryInterface(void)
{
    if(!A(2)||!void_vm_writable(A(2),4))RET(E_POINTER_,3);
    MEM32(A(2))=unknown_iid(A(1))?A(0):0;
    if(MEM32(A(2))) {void_com_addref(void_com_host(A(0)));RET(0,3);}RET(E_NOINTERFACE_,3);
}
#define OK(name,n) static void name(void){RET(0,n);}
#define NO(name,n) static void name(void){void_log("[ddraw] unsupported %s",#name);RET(DD_UNSUPPORTED,n);}
OK(ok1,1) OK(ok2,2) OK(ok3,3) OK(ok4,4) OK(ok5,5)
NO(no2,2) NO(no3,3) NO(no4,4) NO(no5,5) NO(no6,6)
static void dd_CreateClipper(void)
{
    if(!A(2)||!void_vm_writable(A(2),4))RET(E_POINTER_,4);
    dd_clipper *c=calloc(1,sizeof *c);if(!c)RET(E_OUTOFMEMORY_,4);
    MEM32(A(2))=void_com_wrap(&c->com,&clipper_class,0);RET(0,4);
}
static void dd_CreatePalette(void)
{
    if(!A(3)||!void_vm_writable(A(3),4)||!A(2)||!void_vm_readable(A(2),1024))RET(DD_INVALID,5);
    dd_palette *p=calloc(1,sizeof *p);if(!p)RET(E_OUTOFMEMORY_,5);
    memcpy(p->entries,GPTR(A(2)),1024);MEM32(A(3))=void_com_wrap(&p->com,&palette_class,0);RET(0,5);
}
static void dd_CreateSurface(void)
{
    uint32_t p=A(1),out=A(2);if(!p||!void_vm_readable(p,108)||!out||!void_vm_writable(out,4))RET(DD_INVALID,4);
    MEM32(out)=0;uint32_t fl=MEM32(p+4),caps=(fl&1)?MEM32(p+104):0;
    int front=(caps&0x200)!=0,w=front?mode_w:(int)MEM32(p+12),h=front?mode_h:(int)MEM32(p+8);
    int bpp=(!front&&(fl&0x1000))?(int)MEM32(p+84):mode_bpp;
    dd_surface *s=new_surface(w,h,bpp,caps|0x800);if(!s)RET(E_OUTOFMEMORY_,4);
    if((fl&0x800)&&MEM32(p+36)) {
        int pitch=(fl&8)?(int)MEM32(p+16):s->pitch;
        if(pitch<s->pitch||(uint64_t)pitch*h>UINT32_MAX||!void_vm_writable(MEM32(p+36),(uint32_t)pitch*h)) {void_com_release(&s->com);RET(DD_INVALID,4);}
        void_gfree(s->pixels);s->owned=0;s->pixels=MEM32(p+36);s->px=GPTR(s->pixels);s->pitch=pitch;
    }
    if(fl&0x10000){s->key=1;s->key_lo=MEM32(p+64);s->key_hi=MEM32(p+68);}
    if(front) {
        s->primary=1;s->caps|=0x8020;primary=s;
        if((fl&0x20)&&MEM32(p+20)) {s->back=new_surface(w,h,bpp,0x81C);if(!s->back){void_com_release(&s->com);RET(E_OUTOFMEMORY_,4);}s->back->front=s;}
    }
    MEM32(out)=s->com.guest;void_log("[ddraw] surface %08X %dx%dx%d pitch %d caps %08X",s->com.guest,w,h,bpp,s->pitch,caps);RET(0,4);
}
static const int modes[][2]={{800,600},{1024,768},{1280,720},{1280,800},{1280,960},{1366,768},{1600,900},{1920,1080},{2560,1600}};
static void dd_EnumDisplayModes(void)
{
    uint32_t cb=A(4),ctx=A(3),filter=A(2);if(!cb)RET(DD_INVALID,5);
    uint32_t p=void_galloc(124);if(!p)RET(E_OUTOFMEMORY_,5);
    int custom_w=0,custom_h=0,custom=stronghold_display_requested(&custom_w,&custom_h);
    for(unsigned i=0;i<sizeof modes/sizeof modes[0]+custom;i++) {
        int width=i<sizeof modes/sizeof modes[0]?modes[i][0]:custom_w;
        int height=i<sizeof modes/sizeof modes[0]?modes[i][1]:custom_h;
        if(filter&&((MEM32(filter+4)&4)&&MEM32(filter+12)!=(uint32_t)width))continue;
        if(filter&&((MEM32(filter+4)&2)&&MEM32(filter+8)!=(uint32_t)height))continue;
        if(filter&&((MEM32(filter+4)&0x1000)&&MEM32(filter+84)!=16))continue;
        dd_surface s={.w=width,.h=height,.pitch=width*2,.bpp=16};
        MEM32(p)=((dd_device *)void_com_host(A(0)))->version>=4?124:108;description(p,&s);MEM32(p+4)|=0x40000;MEM32(p+24)=60;
        uint32_t args[]={p,ctx};if(!void_call_guest(cb,2,args,1))break;
    }
    void_gfree(p);RET(0,5);
}
static void dd_GetCaps(void)
{
    for(int i=1;i<3;i++){uint32_t p=A(i);if(!p)continue;uint32_t n=MEM32(p);
        if(n<108||n>380||!void_vm_writable(p,n))RET(DD_INVALID,3);
        memset(GPTR(p+4),0,n-4);MEM32(p+4)=0x04404240;MEM32(p+12)=0x200;MEM32(p+60)=256u<<20;MEM32(p+64)=192u<<20;MEM32(p+104)=0xC858;}
    RET(0,3);
}
static void dd_GetDisplayMode(void)
{
    dd_surface s={.w=mode_w,.h=mode_h,.pitch=mode_w*(mode_bpp/8),.bpp=mode_bpp};RET(description(A(1),&s)?0:DD_INVALID,2);
}
static void dd_GetFourCCCodes(void){if(A(1))MEM32(A(1))=0;RET(0,3);}
static void dd_GetGDISurface(void){if(!primary)RET(DD_NOTFOUND,2);MEM32(A(1))=primary->com.guest;void_com_addref(&primary->com);RET(0,2);}
static void dd_GetFrequency(void){if(A(1))MEM32(A(1))=60;RET(0,2);}
static void dd_GetScanLine(void){if(A(1))MEM32(A(1))=(void_tick_ms()*29u)%(uint32_t)mode_h;RET(0,2);}
static void dd_GetBlank(void){if(A(1))MEM32(A(1))=0;RET(0,2);}
static void dd_GetVidMem(void){if(A(2))MEM32(A(2))=256u<<20;if(A(3))MEM32(A(3))=192u<<20;RET(0,4);}
static uint32_t set_mode(void)
{
    int w=(int)A(1),h=(int)A(2),bpp=(int)A(3);if(w<320||h<200||w>8192||h>8192||bpp!=16)return DD_INVALID;
    mode_w=w;mode_h=h;mode_bpp=bpp;void_input_set_screen_size(w,h);void_log("[ddraw] mode %dx%dx%d",w,h,bpp);return 0;
}
static void dd_SetDisplayMode(void){uint32_t r=set_mode();RET(r,4);}
static void dd2_SetDisplayMode(void){uint32_t r=set_mode();RET(r,6);}
static void sf_Blt(void)
{
    dd_surface *d=surface(A(0));dd_rect dr=rect(A(1),d);uint32_t fl=A(4),fx=A(5),result=0;
    if(fl&0x400) {if(!fx||!void_vm_readable(fx,84))RET(DD_INVALID,6);fill(d,dr,MEM32(fx+80));}
    else if(A(2)) {
        dd_surface *s=surface(A(2));int key=(fl&0x8000)&&s->key;uint32_t lo=s->key_lo,hi=s->key_hi;
        if(fl&0x10000){if(!fx||!void_vm_readable(fx,100))RET(DD_INVALID,6);key=1;lo=MEM32(fx+92);hi=MEM32(fx+96);}
        result=blit(d,dr,s,rect(A(3),s),key,lo,hi<lo?lo:hi);
    } else RET(DD_UNSUPPORTED,6);
    if(!result)mark(d);RET(result,6);
}
static void sf_BltFast(void)
{
    if(!A(3))RET(DD_INVALID,6);dd_surface *d=surface(A(0)),*s=surface(A(3));dd_rect sr=rect(A(4),s);
    dd_rect dr={(int)A(1),(int)A(2),(int)A(1)+sr.r-sr.l,(int)A(2)+sr.b-sr.t};
    uint32_t r=blit(d,dr,s,sr,(A(5)&1)&&s->key,s->key_lo,s->key_hi);if(!r)mark(d);RET(r,6);
}
static void sf_Flip(void)
{
    dd_surface *s=surface(A(0)),*b=A(1)?surface(A(1)):s->back;if(!b)RET(DD_NOTFOUND,3);
    if(s->w!=b->w||s->h!=b->h||s->bpp!=b->bpp||s->pitch!=b->pitch)RET(DD_INVALID,3);
    uint8_t *px=s->px;s->px=b->px;b->px=px;uint32_t p=s->pixels;s->pixels=b->pixels;b->pixels=p;
    int owned=s->owned;s->owned=b->owned;b->owned=owned;mark(s);RET(0,3);
}
static void sf_GetAttachedSurface(void)
{
    dd_surface *s=surface(A(0));if(!A(2)||!void_vm_writable(A(2),4))RET(E_POINTER_,3);
    MEM32(A(2))=s->back?s->back->com.guest:0;if(!s->back)RET(DD_NOTFOUND,3);void_com_addref(&s->back->com);RET(0,3);
}
static void sf_AddAttached(void)
{
    dd_surface *s=surface(A(0)),*b=surface(A(1));if(s->back||b->front||s==b)RET(DD_INVALID,2);
    s->back=b;b->front=s;void_com_addref(&b->com);RET(0,2);
}
static void sf_DeleteAttached(void)
{
    dd_surface *s=surface(A(0));if(!s->back||s->back->com.guest!=A(2))RET(DD_NOTFOUND,3);
    dd_surface *b=s->back;s->back=NULL;b->front=NULL;void_com_release(&b->com);RET(0,3);
}
static void sf_GetCaps(void){if(A(1))MEM32(A(1))=surface(A(0))->caps;RET(0,2);}
static void sf_GetColorKey(void)
{
    dd_surface *s=surface(A(0));if(!(A(1)&8)||!s->key)RET(DD_NOCOLORKEY,3);MEM32(A(2))=s->key_lo;MEM32(A(2)+4)=s->key_hi;RET(0,3);
}
static void sf_SetColorKey(void)
{
    dd_surface *s=surface(A(0));if(!(A(1)&8))RET(DD_UNSUPPORTED,3);s->key=A(2)!=0;
    if(s->key){s->key_lo=MEM32(A(2));s->key_hi=MEM32(A(2)+4);}RET(0,3);
}
static void sf_GetFormat(void){pixel_format(A(1),surface(A(0))->bpp);RET(0,2);}
static void sf_GetDesc(void){RET(description(A(1),surface(A(0)))?0:DD_INVALID,2);}
static void sf_Lock(void)
{
    dd_surface *s=surface(A(0));dd_rect r=rect(A(1),s);
    if(r.l<0||r.t<0||r.r>s->w||r.b>s->h||r.l>=r.r||r.t>=r.b||!description(A(2),s))RET(DD_INVALID,5);
    MEM32(A(2)+36)=s->pixels+(uint32_t)r.t*s->pitch+(uint32_t)r.l*(s->bpp/8);s->locks++;RET(0,5);
}
static void sf_Unlock(void){dd_surface *s=surface(A(0));if(!s->locks)RET(DD_NOTLOCKED,2);s->locks--;mark(s);RET(0,2);}
static void sf_SetClipper(void)
{
    dd_surface *s=surface(A(0));uint32_t p=A(1);if(p)void_com_addref(void_com_host(p));
    if(s->clipper)void_com_release(void_com_host(s->clipper));s->clipper=p;RET(0,2);
}
static void sf_GetClipper(void){dd_surface *s=surface(A(0));if(!s->clipper)RET(DD_NOCLIPPER,2);MEM32(A(1))=s->clipper;void_com_addref(void_com_host(s->clipper));RET(0,2);}
static void sf_SetPalette(void)
{
    dd_surface *s=surface(A(0));uint32_t p=A(1);if(p)void_com_addref(void_com_host(p));
    if(s->palette)void_com_release(void_com_host(s->palette));s->palette=p;RET(0,2);
}
static void sf_GetPalette(void){dd_surface *s=surface(A(0));if(!s->palette)RET(DD_NOPALETTE,2);MEM32(A(1))=s->palette;void_com_addref(void_com_host(s->palette));RET(0,2);}
static void sf_GetDC(void)
{
    if(!A(1)||!void_vm_writable(A(1),4))RET(E_POINTER_,2);
    dd_surface *s=surface(A(0));s->dcs++;void_com_addref(&s->com);MEM32(A(1))=s->com.guest;RET(0,2);
}
static void sf_ReleaseDC(void)
{
    dd_surface *s=surface(A(0));if(A(1)!=s->com.guest||!s->dcs)RET(DD_INVALID,2);
    s->dcs--;mark(s);void_com_release(&s->com);RET(0,2);
}
static void sf_GetDD(void){MEM32(A(1))=last_dd;void_com_addref(void_com_host(last_dd));RET(0,2);}
static void cl_GetHWnd(void){MEM32(A(1))=((dd_clipper *)void_com_host(A(0)))->hwnd;RET(0,2);}
static void cl_SetHWnd(void){((dd_clipper *)void_com_host(A(0)))->hwnd=A(2);RET(0,3);}
static void cl_Changed(void){MEM32(A(1))=0;RET(0,2);}
static void pl_GetCaps(void){MEM32(A(1))=4;RET(0,2);}
static void pl_GetEntries(void)
{
    uint32_t start=A(2),n=A(3);if(start>256||n>256-start||!void_vm_writable(A(4),n*4))RET(DD_INVALID,5);
    memcpy(GPTR(A(4)),((dd_palette *)void_com_host(A(0)))->entries+start,n*4);RET(0,5);
}
static void pl_SetEntries(void)
{
    uint32_t start=A(2),n=A(3);if(start>256||n>256-start||!void_vm_readable(A(4),n*4))RET(DD_INVALID,5);
    memcpy(((dd_palette *)void_com_host(A(0)))->entries+start,GPTR(A(4)),n*4);RET(0,5);
}
#define DD_COMMON dd_QueryInterface,addref,release,ok1,dd_CreateClipper,dd_CreatePalette,dd_CreateSurface,no3,dd_EnumDisplayModes,ok5,ok1,dd_GetCaps,dd_GetDisplayMode,dd_GetFourCCCodes,dd_GetGDISurface,dd_GetFrequency,dd_GetScanLine,dd_GetBlank,ok2,ok1,ok3
static const void_fn dd1_methods[]={DD_COMMON,dd_SetDisplayMode,ok3};
static const void_fn dd2_methods[]={DD_COMMON,dd2_SetDisplayMode,ok3,dd_GetVidMem};
static const void_fn dd7_methods[]={DD_COMMON,dd2_SetDisplayMode,ok3,dd_GetVidMem,no3,ok1,ok2,no3,ok2,no3};
static const void_fn surface_methods[]={sf_QueryInterface,addref,release,sf_AddAttached,no2,sf_Blt,no4,sf_BltFast,
    sf_DeleteAttached,no3,ok4,sf_Flip,sf_GetAttachedSurface,ok2,sf_GetCaps,sf_GetClipper,sf_GetColorKey,sf_GetDC,
    ok2,no3,sf_GetPalette,sf_GetFormat,sf_GetDesc,ok3,ok1,sf_Lock,sf_ReleaseDC,ok1,sf_SetClipper,sf_SetColorKey,no3,
    sf_SetPalette,sf_Unlock,no6,no2,no3,sf_GetDD,ok2,ok2,no3,no5,no3,no4,no3,ok2,ok3,ok2,ok2,ok2};
static const void_fn clipper_methods[]={base_QueryInterface,addref,release,no4,cl_GetHWnd,ok3,cl_Changed,no3,cl_SetHWnd};
static const void_fn palette_methods[]={base_QueryInterface,addref,release,pl_GetCaps,pl_GetEntries,ok4,pl_SetEntries};
#define CLASS(name,methods_) static void_com_class name={#name,sizeof methods_/sizeof methods_[0],methods_,0}
CLASS(dd1_class,dd1_methods);CLASS(dd2_class,dd2_methods);CLASS(dd7_class,dd7_methods);
CLASS(surface_class,surface_methods);CLASS(clipper_class,clipper_methods);CLASS(palette_class,palette_methods);

WINAPI_EXPORT("ddraw.dll",DirectDrawCreate,3)
{
    if(!A(1)||!void_vm_writable(A(1),4))return E_POINTER_;
    int width=1280,height=720,custom=stronghold_display_requested(&width,&height);
    if(void_vm_writable(GAME_SCREEN,0x60)&&(custom||first_run_mode(GPTR(GAME_SCREEN+0x5c)))) {
        /* Recalculate all original allocations and HUD offsets before surfaces. */
        MEM32(GAME_SCREEN+0x5c)=3;
        void_call_guest_this(GAME_MODE_CALC,GAME_SCREEN,0,NULL);
        assert(MEM32(GAME_SCREEN+0x18)==(uint32_t)width&&MEM32(GAME_SCREEN+0x1c)==(uint32_t)height&&
               MEM32(GAME_SCREEN+0x40)==(uint32_t)width*2);
        void_log("[ddraw] mobile mode %dx%d (index 3, custom %d)",width,height,custom);
    }
    last_dd=new_device(1);MEM32(A(1))=last_dd;return last_dd?0:E_OUTOFMEMORY_;
}
WINAPI_EXPORT("ddraw.dll",DirectDrawCreateEx,4)
{
    if(!A(1)||!void_vm_writable(A(1),4))return E_POINTER_;
    int version=dd_version(A(2));MEM32(A(1))=0;if(!version)return E_NOINTERFACE_;
    MEM32(A(1))=new_device(version);return MEM32(A(1))?0:E_OUTOFMEMORY_;
}
uint32_t void_ddraw_import(const char *name)
{
    if(!strcmp(name,"DirectDrawCreate"))return void_thunk(DirectDrawCreate_entry,name);
    if(!strcmp(name,"DirectDrawCreateEx"))return void_thunk(DirectDrawCreateEx_entry,name);
    return 0;
}
int void_ddraw_selftest(void)
{
    uint32_t selected=UINT32_MAX;assert(first_run_mode(&selected)&&selected==3);
    selected=2;assert(!first_run_mode(&selected)&&selected==2);
    selected=10;assert(!first_run_mode(&selected)&&selected==10);
    uint16_t a[12]={1,2,3,0xCCCC,4,0,6,0xCCCC,7,8,9,0xCCCC},b[15];
    memset(b,0xAA,sizeof b);dd_surface s={.px=(uint8_t *)a,.w=3,.h=3,.pitch=8,.bpp=16};
    dd_surface d={.px=(uint8_t *)b,.w=4,.h=3,.pitch=10,.bpp=16};
    assert(!blit(&d,(dd_rect){-1,0,2,3},&s,(dd_rect){0,0,3,3},0,0,0));
    assert(b[0]==2&&b[1]==3&&b[5]==0&&b[6]==6&&b[10]==8&&b[11]==9&&b[4]==0xAAAA);
    fill(&d,(dd_rect){0,0,4,3},0xF800);assert(b[0]==0xF800&&b[13]==0xF800&&b[14]==0xAAAA);
    assert(!blit(&d,(dd_rect){0,0,3,3},&s,(dd_rect){0,0,3,3},1,0,0));assert(b[6]==0xF800&&b[7]==6);
    assert(!blit(&s,(dd_rect){1,0,3,3},&s,(dd_rect){0,0,2,3},0,0,0));assert(a[1]==1&&a[2]==2&&a[3]==0xCCCC);
    assert(!blit(&d,(dd_rect){0,0,4,2},&s,(dd_rect){0,0,2,1},0,0,0));assert(b[0]==1&&b[1]==1&&b[2]==1&&b[3]==1);
    assert(blit(&d,(dd_rect){0,0,0,2},&s,(dd_rect){0,0,1,1},0,0,0)==DD_INVALID);
    void_log("[selftest] ddraw CPU: PASS");return 0;
}
