/* Exercise the real surface redirect without creating a game or EGL context. */
#include "../android/host/ddraw.c"
void void_log(const char *format,...){(void)format;}
int stronghold_display_capture_active;
static unsigned capture_calls,capture_bytes;
void stronghold_display_written(uint32_t address,unsigned bytes)
{(void)address;assert(bytes&&!(bytes&1u)&&bytes<=8112);capture_calls++;capture_bytes+=bytes;}
int void_vm_writable(uint32_t address,uint32_t size)
{return address>=0x10000000u&&(uint64_t)address+size<=0x10100000u;}
int main(void)
{
    dd_surface s={.com.guest=0x1234,.pixels=0x10000000,.px=GPTR(0x10000000),
                  .w=16,.h=8,.pitch=32,.bpp=16};
    surfaces=&s;
    assert(!void_ddraw_redirect_pixels(0x4321,0x10010000));
    assert(!void_ddraw_redirect_pixels(s.com.guest,0x0fffffff));
    assert(void_ddraw_redirect_pixels(s.com.guest,0x10010000)==0x10000000);
    assert(s.pixels==0x10010000&&s.px==GPTR(0x10010000));
    s.dcs=1;
    uint32_t pixels;int w,h,pitch,bpp;
    assert(void_ddraw_dc_pixels(s.com.guest,&pixels,&w,&h,&pitch,&bpp));
    assert(pixels==0x10010000&&w==16&&h==8&&pitch==32&&bpp==16);
    assert(!void_ddraw_redirect_pixels(s.com.guest,0x10000000));s.dcs=0;
    s.locks=1;assert(!void_ddraw_redirect_pixels(s.com.guest,0x10000000));s.locks=0;
    s.primary=1;assert(!void_ddraw_redirect_pixels(s.com.guest,0x10000000));s.primary=0;
    s.bpp=32;assert(!void_ddraw_redirect_pixels(s.com.guest,0x10000000));s.bpp=16;
    assert(void_ddraw_redirect_pixels(s.com.guest,0x10000000)==0x10010000);
    assert(s.px==GPTR(0x10000000));surfaces=NULL;
    assert(!void_ddraw_selftest());
    uint16_t source[12]={1,2,3,4,5,6,7,8,9,10,11,12},destination[12]={0};
    dd_surface src={.px=(uint8_t *)source,.w=4,.h=3,.pitch=8,.bpp=16};
    dd_surface dst={.px=(uint8_t *)destination,.w=4,.h=3,.pitch=8,.bpp=16};
    stronghold_display_capture_active=1;
    fill(&dst,(dd_rect){-1,1,5,3},0x1234);
    assert(capture_calls==2&&capture_bytes==16&&destination[0]==0&&destination[4]==0x1234&&destination[11]==0x1234);
    capture_calls=capture_bytes=0;
    assert(!blit(&dst,(dd_rect){-1,0,3,3},&src,(dd_rect){0,0,4,3},0,0,0));
    assert(capture_calls==3&&capture_bytes==18&&destination[0]==2&&destination[10]==12);
    uint8_t wide[18000]={0};dst.px=wide;dst.pitch=9000;dst.h=2;
    capture_calls=capture_bytes=0;fill(&dst,(dd_rect){0,0,4,2},0x4321);
    assert(capture_calls==8&&capture_bytes==16);
    puts("display DirectDraw redirect and batched row coverage: clipping, fill/copy, wide-pitch fallback, overlap/key/scale and surface guards PASS");
    return 0;
}
