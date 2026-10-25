/* Real loopback TCP/UDP transport, COM slots and guest buffer ownership. */
#define _GNU_SOURCE
#include "../android/host/dplay.c"
#include "../android/host/com.c"
#include <assert.h>
#include <sys/mman.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_cur_func;
ptrdiff_t g_mem_base=(ptrdiff_t)VOID_GUEST_BASE;
static uint32_t allocation=0x1000000,callbacks,sessions;
void void_log(const char *format,...){va_list args;va_start(args,format);vprintf(format,args);puts("");va_end(args);}
int void_vm_readable(uint32_t p,uint32_t n){return p>=0x400000&&(uint64_t)p+n<=0x4000000;}
int void_vm_writable(uint32_t p,uint32_t n){return void_vm_readable(p,n);}
uint32_t void_galloc(uint32_t n){uint32_t p=allocation;allocation+=(n+15)&~15u;assert(allocation<0x4000000);memset(GPTR(p),0,n);return p;}
void void_gfree(uint32_t p){(void)p;}
uint32_t void_thunk(void_fn fn,const char *name){(void)fn;(void)name;static uint32_t address=GUEST_THUNK_BASE;return address+=4;}
_Noreturn void void_fatal(const char *format,...){(void)format;abort();}
_Noreturn void void_lift_trap(uint32_t p,const char *reason){fprintf(stderr,"trap %x %s\n",p,reason);abort();}
uint64_t void_monotonic_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
void void_blocking_begin(void){}
void void_blocking_end(void){}
uint32_t void_call_guest(uint32_t fn,int n,const uint32_t *args,int pops)
{
    assert(pops==1);callbacks++;
    if(fn==1){assert(n==6&&!memcmp(GPTR(args[0]),guid_tcp,16)&&MEM32(args[3]+8));return 1;}
    assert(fn==2&&n==4);
    if(args[2]==0){assert(readable(args[0],80)&&MEM32(args[0])==80&&MEM32(args[0]+48));sessions++;return 1;}
    assert(args[2]==1&&args[0]==0);return 0;
}
static uint32_t invoke(dp_object *d,unsigned slot,int n,...)
{
    uint32_t stack=0x410000;g_esp=stack;MEM32(stack)=0x12345678;MEM32(stack+4)=d->com.guest;
    va_list args;va_start(args,n);for(int i=1;i<n;i++)MEM32(stack+4+4*i)=va_arg(args,uint32_t);va_end(args);
    const void_fn *methods=d->lobby?lobby_methods:dp_methods;methods[slot]();assert(g_esp==stack+4+4*(uint32_t)n);return g_eax;
}
static void wait_messages(dp_object *d,unsigned expected)
{
    uint64_t end=void_monotonic_ns()+2000000000u;
    while(void_monotonic_ns()<end){pthread_mutex_lock(&d->lock);unsigned n=d->messages;pthread_mutex_unlock(&d->lock);if(n>=expected)return;poll(NULL,0,5);}
    assert(!"network message deadline");
}
static void drain(dp_object *d)
{
    uint32_t p=void_galloc(1024*1024),size=void_galloc(4),from=void_galloc(4),to=void_galloc(4);
    for(;;){MEM32(size)=1024*1024;uint32_t r=invoke(d,25,6,from,to,1u,p,size);if(r==DP_NO_MESSAGES)break;assert(r==0);}
}
int main(void)
{
    assert(mmap((void *)(VOID_GUEST_BASE+0x400000),0x3c00000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
    dp_object *host=new_dp(0,1),*client=new_dp(0,1),*lobby=new_dp(1,1);assert(host&&client&&lobby&&stronghold_dplay_active());
    uint32_t iid=void_galloc(16),object=void_galloc(4);
    const uint32_t dp4[]={0x0AB1C530,0x11D14745,0x0000A1A7,0xFCAB03F8};memcpy(GPTR(iid),dp4,16);
    assert(!invoke(host,0,3,iid,object)&&MEM32(object)==host->com.guest);invoke(host,2,1);
    MEM8(iid+15)^=1;assert(invoke(host,0,3,iid,object)==0x80004002&&!MEM32(object));
    uint32_t connection=void_galloc(64);memcpy(GPTR(connection),guid_sp,16);MEM32(connection+16)=16;memcpy(GPTR(connection+20),guid_tcp,16);
    assert(!invoke(host,38,3,connection,0u));assert(!invoke(host,35,5,0u,1u,0u,0u)&&callbacks==1);
    uint32_t desc=void_galloc(512),name=desc+80;MEM32(desc)=80;MEM32(desc+40)=8;MEM32(desc+48)=name;
    for(int i=0;i<4;i++)MEM32(desc+24+4*i)=0x12340000u+(unsigned)i;
    MEM16(name)=0x0422;MEM16(name+2)='S';assert(!invoke(host,24,3,desc,2u));
    uint32_t player_name=void_galloc(512),hostid=void_galloc(4),clientid=void_galloc(4);
    MEM32(player_name)=16;MEM32(player_name+8)=player_name+16;MEM16(player_name+16)=0x042F;MEM16(player_name+18)='A';
    assert(!invoke(host,6,7,hostid,player_name,0u,0u,0u,0u)&&MEM32(hostid)==1);
    memcpy(GPTR(connection),guid_size,16);MEM32(connection+16)=4;MEM32(connection+20)=54;
    memcpy(GPTR(connection+24),guid_inet,16);MEM32(connection+40)=10;memcpy(GPTR(connection+44),"127.0.0.1",10);
    assert(!invoke(client,38,3,connection,0u));assert(!invoke(client,13,6,desc,0u,2u,0u,1u));assert(sessions>=1&&client->found_count>=1);
    unsigned before_stop=callbacks;
    assert(!invoke(client,13,6,desc,0u,2u,0u,0x20u)&&callbacks==before_stop);
    memcpy(GPTR(desc),host->session.desc,80);MEM32(desc+48)=name;
    assert(!invoke(client,24,3,desc,1u));assert(!invoke(client,6,7,clientid,player_name,0u,0u,0u,0u));assert(MEM32(clientid)==2);
    wait_messages(host,1);drain(host);drain(client);
    uint32_t payload=void_galloc(200000),size=void_galloc(4),from=void_galloc(4),to=void_galloc(4),out=void_galloc(200000);
    for(unsigned i=0;i<200000;i++)MEM8(payload+i)=(uint8_t)(i*23);
    for(unsigned i=1;i<=3;i++)assert(!invoke(client,26,6,2u,1u,1u,payload,i*60000u));
    wait_messages(host,3);
    for(unsigned i=1;i<=3;i++){
        MEM32(size)=1;assert(invoke(host,25,6,from,to,1u,out,size)==DP_BUFFER_SMALL&&MEM32(size)==i*60000);
        MEM32(size)=200000;assert(!invoke(host,25,6,from,to,9u,out,size));assert(!memcmp(GPTR(payload),GPTR(out),i*60000));
        MEM32(size)=200000;assert(!invoke(host,25,6,from,to,1u,out,size)&&MEM32(from)==2&&MEM32(to)==1&&MEM32(size)==i*60000);
        assert(!memcmp(GPTR(payload),GPTR(out),i*60000));
    }
    MEM32(size)=200000;assert(invoke(host,25,6,from,to,1u,out,size)==DP_NO_MESSAGES);
    assert(invoke(client,26,6,1u,1u,1u,payload,1u)==DP_NO_CONNECTION);
    assert(invoke(client,26,6,2u,1u,1u,payload,LAN_MAX_DATA+1)==DP_INVALID);
    pthread_mutex_lock(&host->lock);assert(!process_packet(host,0,NET_DATA,99,1,GPTR(payload),4));pthread_mutex_unlock(&host->lock);
    uint32_t elements=void_galloc(24),address=void_galloc(256);memcpy(GPTR(elements),guid_inet,16);MEM32(elements+16)=10;MEM32(elements+20)=connection+44;
    MEM32(size)=0;assert(invoke(lobby,14,5,elements,1u,0u,size)==DP_BUFFER_SMALL&&MEM32(size)==54);
    MEM32(size)=256;assert(!invoke(lobby,14,5,elements,1u,address,size));assert(MEM32(address+40)==10&&MEM32(address+20)==54);
    assert(!invoke(client,38,3,address,0u));MEM32(address+20)=4095;MEM32(address+40)=4076;
    assert(invoke(client,38,3,address,0u)==DP_INVALID);
    MEM32(elements+16)=5000;assert(invoke(lobby,14,5,elements,1u,address,size)==DP_INVALID);
    assert(!invoke(client,4,1));wait_messages(host,1);MEM32(size)=200000;assert(!invoke(host,25,6,from,to,1u,out,size)&&MEM32(out)==5&&MEM32(out+8)==2);
    assert(!invoke(client,24,3,desc,1u));assert(!invoke(client,6,7,clientid,player_name,0u,0u,0u,0u));
    drain(client);assert(!invoke(host,4,1));wait_messages(client,1);
    uint64_t deadline=void_monotonic_ns()+2000000000u;while(!atomic_load(&disconnected)&&void_monotonic_ns()<deadline)poll(NULL,0,5);
    assert(stronghold_dplay_disconnected()&&!stronghold_dplay_disconnected());
    MEM32(size)=200000;assert(!invoke(client,25,6,from,to,1u,out,size)&&MEM32(out)==0x31);
    assert(!invoke(client,2,1));assert(!invoke(host,2,1));assert(!invoke(lobby,2,1));assert(!stronghold_dplay_active());
    puts("DirectPlay COM slots, Unicode names, UDP discovery, TCP join/player notifications, 60/120/180KB reliable FIFO, sizing/peek, sender validation, malformed address and disconnect: PASS");
    return 0;
}
