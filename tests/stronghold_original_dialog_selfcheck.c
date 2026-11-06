/* Original modals shade full-frame coordinates after a smaller zoom render. */
#define STRONGHOLD_ORIGINAL
#include "../android/host/stronghold_original.c"
#include <assert.h>
#include <sys/mman.h>

static void page(uint32_t address)
{
    address &= ~4095u;
    assert(mmap(GPTR(address),4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==GPTR(address));
}
int main(void)
{
    page(SCREEN_WIDTH); page(RASTER_ORIGIN); page(0x1503CB8u); page(0x18ECCE8u);
    const uint32_t world=0x30000000u, object=0x1503CB8u;
    assert(mmap(GPTR(world),RASTER_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==GPTR(world));
    frame_width=800;frame_height=600;origin_x=13;origin_y=7;MEM32(WORLD_PIXELS)=world;
    world_copy=calloc((size_t)frame_width*frame_height,2);assert(world_copy);
    MEM32(object+8)=450;MEM32(object+12)=200;MEM32(object+16)=300;MEM32(object+20)=200;MEM32(object+0x2C)=5;
    MEM32(0x18ECCE8u+0x2C)=UINT32_MAX;
    for(int native=0;native<2;native++) for(int zoom=0;zoom<3;zoom++) {
        const float scales[]={1.25f,2.f,2.5f};float scale=scales[zoom];
        MEM32(CAMERA+0x8C)=native;render_world_zoom=scale/(native?2.f:1.f);
        memset(world_copy,0,(size_t)frame_width*frame_height*2);
        for(int y=0;y<frame_height;y++) memset(raster_row(world,y),0,(size_t)frame_width*2);
        for(int y=0;y<(int)ceilf(frame_height/scale);y++) for(int x=0;x<(int)ceilf(frame_width/scale);x++)
            world_copy[y*frame_width+x]=(uint16_t)(0x0841u+((x+y)&31));
        assert(raster_row(world,350)[700]==0); /* Old dialog background reads black here. */
        dialog_background();
        assert(raster_row(world,350)[700]==world_copy[(int)(350/scale)*frame_width+(int)(700/scale)]);
        assert(raster_row(world,350)[700]!=0 && raster_row(world,199)[700]==0);
        assert(world_copy[350*frame_width+700]==0); /* GPU source remains untouched. */
    }
    render_world_zoom=1;MEM32(CAMERA+0x8C)=0;
    raster_row(world,350)[700]=0xBEEF;dialog_background();assert(raster_row(world,350)[700]==0xBEEF);
    puts("HD pause background: six smooth/native zoom combinations, black unrendered source, exact samples, unchanged GPU world, raster origin and 100% view PASS");
    return 0;
}
