/* Exercise the real surface redirect without creating a game or EGL context. */
#include "../android/host/renderer.c"
#include <sys/mman.h>

static const uint32_t scratch=0x10000000u, stack=0x10000100u;
static uint32_t bump=0x10001000u;
static unsigned allocated,presented;
static uint16_t last_pixel;
ptrdiff_t g_mem_base=(ptrdiff_t)VOID_GUEST_BASE;
uint32_t g_eax,g_esp;
void void_log(const char *format,...){(void)format;}
int stronghold_display_capture_active;
static unsigned capture_calls,capture_bytes;
void stronghold_display_written(uint32_t address,unsigned bytes)
{(void)address;assert(bytes&&!(bytes&1u)&&bytes<=8112);capture_calls++;capture_bytes+=bytes;}
int void_vm_writable(uint32_t address,uint32_t size)
{return address>=0x10000000u&&(uint64_t)address+size<=0x10100000u;}
int void_vm_readable(uint32_t address,uint32_t size){return void_vm_writable(address,size);}
uint32_t void_galloc(uint32_t size)
{
    uint32_t addr=bump;
    bump+=(size+15u)&~15u;
    assert(void_vm_writable(addr,size));
    ++allocated;
    return addr;
}
void void_gfree(uint32_t addr){assert(allocated&&addr>=0x10001000u&&addr<bump);--allocated;}
void void_input_set_screen_size(int w,int h){assert(w==320&&h==200);}
void stronghold_renderer_present_pixels(const uint8_t *pixels,int w,int h,int pitch,int bpp)
{
    assert(w==320&&h==200&&pitch==640&&bpp==16);
    last_pixel=((const uint16_t *)pixels)[0];
    ++presented;
}

static uint32_t invoke(void (*operation)(void),unsigned nargs,const uint32_t *args)
{
    g_esp=stack;
    for(unsigned i=0;i<nargs;++i) MEM32(stack+4+4*i)=args[i];
    operation();
    assert(g_esp==stack+4+4*nargs);
    return g_eax;
}

static void renderer_lifetime(void)
{
    void *mapped=mmap(GPTR(scratch),0x100000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    assert(mapped==GPTR(scratch));
    const uint32_t desc=scratch, front_out=scratch+108, back_out=scratch+112;
    assert(!invoke(stronghold_render_set_mode,4,(uint32_t[]){1,320,200,16}));
    MEM32(desc)=108;MEM32(desc+4)=0x21;MEM32(desc+20)=1;MEM32(desc+104)=0x200;
    assert(!invoke(stronghold_render_allocate,4,(uint32_t[]){1,desc,front_out,0}));
    uint32_t front=MEM32(front_out);
    assert(!invoke(stronghold_render_back_buffer,3,(uint32_t[]){front,0,back_out}));
    uint32_t back=MEM32(back_out);

    assert(!invoke(stronghold_render_format,2,(uint32_t[]){back,desc}));
    assert(MEM32(desc+12)==16&&MEM32(desc+16)==0xF800&&MEM32(desc+20)==0x7E0&&MEM32(desc+24)==31);
    MEM32(desc)=108;
    assert(!invoke(stronghold_render_lock,5,(uint32_t[]){back,0,desc,1,0}));
    assert(MEM32(desc+16)==640);
    MEM16(MEM32(desc+36))=0x07E0;
    assert(!invoke(stronghold_render_unlock,2,(uint32_t[]){back,0}));
    assert(!presented);
    assert(!invoke(stronghold_render_swap,3,(uint32_t[]){front,0,1}));
    assert(presented==1&&last_pixel==0x07E0);

    assert(!invoke(stronghold_render_acquire_dc,2,(uint32_t[]){front,desc}));
    assert(MEM32(desc)==front);
    assert(invoke(stronghold_render_release,1,(uint32_t[]){back})==1&&surface(back));
    assert(invoke(stronghold_render_release,1,(uint32_t[]){front})==1);
    assert(!invoke(stronghold_render_release_dc,2,(uint32_t[]){front,front}));
    assert(presented==2&&!surfaces&&!primary);
    assert(!allocated);

    const uint32_t external=scratch+0xF0000;
    memset(GPTR(desc),0,108);
    MEM32(desc)=108;MEM32(desc+4)=0x80F;MEM32(desc+8)=8;MEM32(desc+12)=16;MEM32(desc+36)=external;
    MEM32(desc+16)=30;
    assert(invoke(stronghold_render_allocate,4,(uint32_t[]){1,desc,front_out,0})==E_INVALIDARG_);
    assert(!allocated&&!MEM32(front_out));
    MEM32(desc+16)=32;
    assert(!invoke(stronghold_render_allocate,4,(uint32_t[]){1,desc,front_out,0}));
    uint32_t layer=MEM32(front_out);
    assert(stronghold_renderer_redirect(layer,external+0x1000)==external);
    assert(!invoke(stronghold_render_release,1,(uint32_t[]){layer}));
    assert(!allocated&&!surfaces);
    munmap(mapped,0x100000);
}

int main(void)
{
    render_surface s={.token=0x1234,.pixels=0x10000000,.px=GPTR(0x10000000),
                  .w=16,.h=8,.pitch=32,.bpp=16};
    surfaces=&s;
    assert(!stronghold_renderer_redirect(0x4321,0x10010000));
    assert(!stronghold_renderer_redirect(s.token,0x0fffffff));
    assert(stronghold_renderer_redirect(s.token,0x10010000)==0x10000000);
    assert(s.pixels==0x10010000&&s.px==GPTR(0x10010000));
    s.dcs=1;
    uint32_t pixels;int w,h,pitch,bpp;
    assert(stronghold_renderer_dc_pixels(s.token,&pixels,&w,&h,&pitch,&bpp));
    assert(pixels==0x10010000&&w==16&&h==8&&pitch==32&&bpp==16);
    assert(!stronghold_renderer_redirect(s.token,0x10000000));s.dcs=0;
    s.locks=1;assert(!stronghold_renderer_redirect(s.token,0x10000000));s.locks=0;
    s.primary=1;assert(!stronghold_renderer_redirect(s.token,0x10000000));s.primary=0;
    s.bpp=32;assert(!stronghold_renderer_redirect(s.token,0x10000000));s.bpp=16;
    assert(stronghold_renderer_redirect(s.token,0x10000000)==0x10010000);
    assert(s.px==GPTR(0x10000000));surfaces=NULL;
    assert(!stronghold_renderer_selftest());
    uint16_t source[12]={1,2,3,4,5,6,7,8,9,10,11,12},destination[12]={0};
    render_surface src={.px=(uint8_t *)source,.w=4,.h=3,.pitch=8,.bpp=16};
    render_surface dst={.px=(uint8_t *)destination,.w=4,.h=3,.pitch=8,.bpp=16};
    stronghold_display_capture_active=1;
    fill(&dst,(render_rect){-1,1,5,3},0x1234);
    assert(capture_calls==2&&capture_bytes==16&&destination[0]==0&&destination[4]==0x1234&&destination[11]==0x1234);
    capture_calls=capture_bytes=0;
    assert(!blit(&dst,(render_rect){-1,0,3,3},&src,(render_rect){0,0,4,3},0,0,0));
    assert(capture_calls==3&&capture_bytes==18&&destination[0]==2&&destination[10]==12);
    uint8_t wide[18000]={0};dst.px=wide;dst.pitch=9000;dst.h=2;
    capture_calls=capture_bytes=0;fill(&dst,(render_rect){0,0,4,2},0x4321);
    assert(capture_calls==8&&capture_bytes==16);
    stronghold_display_capture_active=0;
    renderer_lifetime();
    puts("native renderer: RGB565, clipping, overlap/key/scale, HUD redirect, lock, swap, HDC and buffer lifetime PASS");
    return 0;
}
