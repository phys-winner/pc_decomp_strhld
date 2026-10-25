/* Exact output checks and a small device benchmark for the five CPU changes. */
#define STRONGHOLD_DISPLAY_SELFTEST
#define main display_selfcheck_main
#ifndef DISPLAY_SOURCE
#define DISPLAY_SOURCE "../android/host/display.c"
#endif
#include DISPLAY_SOURCE
#undef main
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000.0+ts.tv_nsec/1000000.0;
}

int main(void)
{
    assert(!display_selfcheck_main());
    uint8_t a[2112],b[2112];size_t first,last;
    for(size_t offset=0;offset<16;offset++)for(size_t n=0;n<=2049;n+=17){
        memset(a,7,sizeof a);memset(b,7,sizeof b);
        assert(!copy_changed(a+offset,b+offset,n,&first,&last));
        if(!n)continue;
        for(size_t k=0;k<n;k+=31){
            b[offset+k]^=1;
            assert(copy_changed(a+offset,b+offset,n,&first,&last)&&first==k&&last==k+1);
            assert(!memcmp(a,b,sizeof a));
        }
    }
    for(size_t n=0;n<=2049;n+=7){
        memset(a,0,sizeof a);assert(!any_set(a+3,n));
        for(size_t k=0;k<n;k+=19){a[3+k]=128;assert(any_set(a+3,n));a[3+k]=0;}
    }
    stronghold_display_configure(1200,600);assert(allocate_frames());
    original_pixels=0x16000000;origin_x=13;origin_y=7;source_width=1200;source_height=600;
    for(int y=0;y<600;y++)for(int x=0;x<1200;x++)
        MEM16(original_pixels+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)(origin_x+x))=(uint16_t)(x*17+y*31);
    for(int mode=0;mode<3;mode++){
        render_scale=mode==0?1:mode==1?.75f:.5f;capture_world();
        for(int y=0;y<raster_height;y++)for(int x=0;x<raster_width;x++)
            assert(world_pixels[y*1200+x]==(uint16_t)(((int64_t)x*1200/raster_width)*17+((int64_t)y*600/raster_height)*31));
        double start=now_ms();for(int k=0;k<60;k++)capture_world();
        printf("capture scale %.2f: %.4f ms\n",render_scale,(now_ms()-start)/60);
    }
    ui_layer *l=&ui[4];l->guest=0x18000000;original_surface=1;color_key=0xf81f;
    for(int step=0;step<12;step++){
        if(step==4)origin_x++;
        if(step==8)color_key=0x07e0;
        l->used=0;assert(select_group(4));ui_open=1;
        for(int y=0;y<600;y++)for(int x=0;x<1200;x++){
            assert(!l->capture[y*1200+x]);
            assert(MEM16(l->guest+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)(origin_x+x))==color_key);
        }
        int x=200+step*9,y=100+step*7;
        uint32_t at=l->guest+MAP_PITCH*(uint32_t)(origin_y+y)+2u*(uint32_t)(origin_x+x);
        MEM16(at)=0x1234;stronghold_display_written(at,2);
        if(step%2)publish_layer(4); /* Also clear a capture aborted before publication. */
    }
    l->used=1;l->left=0;l->top=0;l->right=1200;l->bottom=600;
    memset(l->capture,0,720000);memset(l->coverage,0,720000);
    l->shown=(stronghold_rect){0,0,1200,600};
    double start=now_ms();for(int k=0;k<200;k++)publish_layer(4);
    printf("transparent publication: %.4f ms\n",(now_ms()-start)/200);
    start=now_ms();size_t total=0;
    memset(a,7,sizeof a);memset(b,7,sizeof b);
    for(int k=0;k<100000;k++){b[1024]^=1;total+=copy_changed(a+3,b+3,2049,&first,&last);}
    assert(total==100000);
    printf("changed-row copy: %.6f ms\n",(now_ms()-start)/100000);
    puts("CPU performance: zero/unaligned rows, scaled capture, hidden RGB, moved/keyed/origin/aborted layer reset PASS");
    return 0;
}
