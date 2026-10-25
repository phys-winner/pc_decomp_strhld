/* DirectPlay 2/3/4 guest ABI over a bounded, reliable Android LAN transport.
 * Interface layouts: Wine include/dplay.h and dplobby.h. No Windows driver is used. */
#include "host.h"
#include "com.h"
#include "dplay.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DP_BUFFER_SMALL 0x8877001Eu
#define DP_NO_MESSAGES 0x887700BEu
#define DP_INVALID_PLAYER 0x88770096u
#define DP_NO_CONNECTION 0x887700AAu
#define DP_UNSUPPORTED 0x80004001u
#define DP_INVALID 0x80070057u
#define LAN_MAGIC 0x53484C37u
#ifdef STRONGHOLD_ORIGINAL
#define LAN_PORT 47628 /* Stronghold HD: its own pair, beside Crusader's */
#elif defined(STRONGHOLD_CLASSIC)
#define LAN_PORT 47624
#else
#define LAN_PORT 47625
#endif
#define LAN_DISCOVERY (LAN_PORT+2)
#define LAN_MAX_DATA (1024u*1024u)
#define LAN_QUEUE_BYTES (8u*1024u*1024u)
#define LAN_PLAYERS 8
#define LAN_NAME 128
#define LAN_PLAYER_DATA 4096
enum { NET_HELLO=1,NET_WELCOME,NET_PLAYER,NET_REMOVE,NET_DATA,NET_SESSION,NET_PLAYER_DATA };
typedef struct { uint32_t desc[20];uint16_t name[LAN_NAME]; } lan_session;
typedef struct { uint32_t id,flags,length;uint16_t name[LAN_NAME];uint8_t data[LAN_PLAYER_DATA]; } lan_player;
typedef struct lan_packet { struct lan_packet *next;size_t size,sent;uint8_t bytes[]; } lan_packet;
typedef struct lan_message { struct lan_message *next;uint32_t kind,from,to,size;uint8_t bytes[]; } lan_message;
typedef struct {
    int fd,welcomed;uint32_t id;uint8_t *input;size_t used,capacity,queued;
    uint64_t accepted;
    lan_packet *first,*last;
} lan_peer;
typedef struct {
    void_com_object com;
    int lobby,wide,initialized,open,hosting,stop,worker_started,welcomed,lost,loss_reported;
    int listener,discovery;pthread_t worker;pthread_mutex_t lock;pthread_cond_t ready;
    uint32_t local_id,next_id;
    char address[64];lan_session session;
    lan_player players[LAN_PLAYERS];
    uint32_t local_lengths[LAN_PLAYERS];uint8_t local_data[LAN_PLAYERS][LAN_PLAYER_DATA];
    lan_peer peers[LAN_PLAYERS];
    lan_message *first,*last;size_t messages,queued;
    struct { lan_session session;struct in_addr ip; } found[32];unsigned found_count;
} dp_object;
static _Atomic int dp_objects;
static _Atomic int disconnected;
static const uint8_t guid_dp[16]={0x20,0x6d,0xeb,0xd1,0x23,0x89,0xd0,0x11,0x9d,0x97,0,0xa0,0xc9,0x0a,0x43,0xcb};
static const uint8_t guid_lobby[16]={0x10,0xf8,0xe8,0x2f,0xa5,0xb2,0xd0,0x11,0xa7,0x87,0,0,0xf8,3,0xab,0xfc};
static const uint8_t guid_tcp[16]={0xe0,0x5e,0xe9,0x36,0x77,0x85,0xcf,0x11,0x96,0x0c,0,0x80,0xc7,0x53,0x4e,0x82};
static const uint8_t guid_inet[16]={0xa0,0x4d,0xa5,0xc4,0xaf,0xe0,0xcf,0x11,0x9c,0x4e,0,0xa0,0xc9,5,0x42,0x5e};
static const uint8_t guid_sp[16]={0xc0,0x16,0xd9,0x07,0xaf,0xe0,0xcf,0x11,0x9c,0x4e,0,0xa0,0xc9,5,0x42,0x5e};
static const uint8_t guid_size[16]={0x60,0xf5,0x18,0x13,0x2c,0x91,0xd0,0x11,0x9d,0xaa,0,0xa0,0xc9,0x0a,0x43,0xcb};
static void_com_class dp_class,lobby_class;
int stronghold_dplay_active(void){return atomic_load(&dp_objects)>0;}
int stronghold_dplay_disconnected(void){return atomic_exchange(&disconnected,0);}
static int readable(uint32_t p,uint32_t n){return p&&void_vm_readable(p,n);}
static int writable(uint32_t p,uint32_t n){return p&&void_vm_writable(p,n);}
static void get_name(uint32_t p,int wide,uint16_t name[LAN_NAME])
{
    unsigned n=0;memset(name,0,LAN_NAME*2);
    for(;n<LAN_NAME-1&&readable(p+n*(wide?2:1),wide?2:1);n++) {
        name[n]=wide?MEM16(p+n*2):MEM8(p+n);if(!name[n])break;
    }
}
static size_t name_size(const uint16_t *name,int wide)
{size_t n=0;while(n<LAN_NAME-1&&name[n])n++;return (n+1)*(wide?2:1);}
static void put_name(uint32_t p,const uint16_t *name,int wide)
{
    size_t n=name_size(name,wide)/(wide?2:1);
    for(size_t i=0;i<n;i++){if(wide)MEM16(p+(uint32_t)i*2)=name[i];else MEM8(p+(uint32_t)i)=name[i]<256?name[i]:'?';}
}
static int player_index(dp_object *d,uint32_t id)
{for(int i=0;i<LAN_PLAYERS;i++)if(d->players[i].id==id&&id)return i;return -1;}
static unsigned player_count(dp_object *d)
{unsigned n=0;for(int i=0;i<LAN_PLAYERS;i++)n+=d->players[i].id!=0;return n;}
static void refresh_session(dp_object *d){d->session.desc[11]=player_count(d);}
static int nonblocking(int fd)
{int f=fcntl(fd,F_GETFL,0);return f>=0&&fcntl(fd,F_SETFL,f|O_NONBLOCK)>=0;}
static int make_socket(int type,int port)
{
    int fd=socket(AF_INET,type,0),one=1;if(fd<0)return -1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    if(type==SOCK_DGRAM)setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&one,sizeof one);
    else setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons((uint16_t)port),.sin_addr.s_addr=INADDR_ANY};
    if(!nonblocking(fd)||(port&&bind(fd,(struct sockaddr *)&a,sizeof a))){close(fd);return -1;}
    return fd;
}
static int queue_packet(lan_peer *p,uint32_t kind,uint32_t from,uint32_t to,const void *data,uint32_t size)
{
    if(size>LAN_MAX_DATA||p->queued+size+20>LAN_QUEUE_BYTES)return 0;
    lan_packet *q=malloc(sizeof *q+20+size);if(!q)return 0;
    q->next=NULL;q->sent=0;q->size=20+size;
    uint32_t words[]={htonl(LAN_MAGIC),htonl(kind),htonl(from),htonl(to),htonl(size)};
    memcpy(q->bytes,words,20);if(size)memcpy(q->bytes+20,data,size);
    if(p->last)p->last->next=q;else p->first=q;p->last=q;p->queued+=q->size;return 1;
}
static int queue_message(dp_object *d,uint32_t kind,uint32_t from,uint32_t to,const void *data,uint32_t size)
{
    if(d->messages>=4096||size>LAN_MAX_DATA||d->queued+size+sizeof(lan_message)>LAN_QUEUE_BYTES){d->lost=1;return 0;}
    lan_message *m=malloc(sizeof *m+size);if(!m){d->lost=1;return 0;}
    *m=(lan_message){.kind=kind,.from=from,.to=to,.size=size};if(size)memcpy(m->bytes,data,size);
    if(d->last)d->last->next=m;else d->first=m;d->last=m;d->messages++;d->queued+=size+sizeof *m;return 1;
}
static void relay(dp_object *d,int except,uint32_t kind,uint32_t from,uint32_t to,const void *data,uint32_t size)
{
    for(int i=0;i<LAN_PLAYERS;i++)if(i!=except&&d->peers[i].fd>=0&&d->peers[i].welcomed&&
        (!to||d->peers[i].id==to))if(!queue_packet(&d->peers[i],kind,from,to,data,size))d->lost=1;
}
static void drop_peer(dp_object *d,int index)
{
    lan_peer *p=&d->peers[index];uint32_t id=p->id;
    if(p->fd>=0)close(p->fd);free(p->input);
    while(p->first){lan_packet *q=p->first;p->first=q->next;free(q);}
    memset(p,0,sizeof *p);p->fd=-1;
    if(d->hosting) {
        int n=player_index(d,id);if(n>=0){memset(&d->players[n],0,sizeof(lan_player));refresh_session(d);
            queue_message(d,5,0,d->local_id,&id,4);relay(d,index,NET_REMOVE,0,0,&id,4);
            void_log("[lan] player %u disconnected",id);}
    } else if(d->open&&!d->stop){d->lost=1;queue_message(d,0x31,0,d->local_id,NULL,0);pthread_cond_broadcast(&d->ready);}
}
static int valid_session(const lan_session *s)
{return s->desc[0]==80&&s->desc[10]>0&&s->desc[10]<=LAN_PLAYERS&&s->desc[11]<=LAN_PLAYERS&&s->name[LAN_NAME-1]==0;}
static int valid_player(const lan_player *p)
{return p->id>0&&p->length<=LAN_PLAYER_DATA&&p->name[LAN_NAME-1]==0;}
static int process_packet(dp_object *d,int index,uint32_t kind,uint32_t from,uint32_t to,const uint8_t *data,uint32_t size)
{
    lan_peer *peer=&d->peers[index];
    if(d->hosting&&!peer->welcomed) {
        if(kind!=NET_HELLO||size!=32||memcmp(data,d->session.desc+2,32)||
           (d->session.desc[1]&0x21)||player_count(d)>=d->session.desc[10])return 0;
        uint8_t welcome[4+sizeof(lan_session)+sizeof d->players];
        memcpy(welcome,&peer->id,4);memcpy(welcome+4,&d->session,sizeof d->session);
        memcpy(welcome+4+sizeof d->session,d->players,sizeof d->players);
        peer->welcomed=1;return queue_packet(peer,NET_WELCOME,0,peer->id,welcome,sizeof welcome);
    }
    if(!d->hosting&&!d->welcomed) {
        if(kind!=NET_WELCOME||size!=4+sizeof(lan_session)+sizeof d->players)return 0;
        lan_session session;memcpy(&session,data+4,sizeof session);
        if(!valid_session(&session)||memcmp(session.desc+2,d->session.desc+2,32))return 0;
        uint32_t id;memcpy(&id,data,4);if(id<2)return 0;
        memcpy(d->players,data+4+sizeof session,sizeof d->players);
        for(int i=0;i<LAN_PLAYERS;i++)if(d->players[i].id&&!valid_player(&d->players[i]))return 0;
        d->session=session;d->local_id=id;peer->welcomed=d->welcomed=1;pthread_cond_broadcast(&d->ready);return 1;
    }
    if(d->hosting&&from!=peer->id)return 0;
    if(kind==NET_DATA) {
        if(!from||player_index(d,from)<0)return 0;
        if(!to||to==d->local_id)queue_message(d,0,from,d->local_id,data,size);
        if(d->hosting)relay(d,index,kind,from,to,data,size);return 1;
    }
    if(kind==NET_PLAYER||kind==NET_PLAYER_DATA) {
        lan_player player;if(size!=sizeof player)return 0;memcpy(&player,data,sizeof player);
        if(!valid_player(&player)||(d->hosting&&player.id!=peer->id))return 0;
        int n=player_index(d,player.id),created=n<0;
        if(n<0)for(int j=0;j<LAN_PLAYERS;j++)if(!d->players[j].id){n=j;break;}
        if(n<0)return 0;d->players[n]=player;refresh_session(d);
        if(player.id!=d->local_id)queue_message(d,created?3:0x102,0,d->local_id,&player,sizeof player);
        if(d->hosting)relay(d,index,kind,from,0,data,size);
        if(created)void_log("[lan] player %u joined (%u players)",player.id,player_count(d));return 1;
    }
    if(kind==NET_REMOVE&&!d->hosting&&size==4) {
        uint32_t id;memcpy(&id,data,4);int n=player_index(d,id);
        if(n>=0){memset(&d->players[n],0,sizeof(lan_player));refresh_session(d);queue_message(d,5,0,d->local_id,&id,4);}return 1;
    }
    if(kind==NET_SESSION&&!d->hosting&&size==sizeof(lan_session)) {
        lan_session session;memcpy(&session,data,sizeof session);
        if(!valid_session(&session)||memcmp(session.desc+2,d->session.desc+2,32))return 0;
        d->session=session;queue_message(d,0x104,0,d->local_id,data,size);return 1;
    }
    return 0;
}
static int peer_io(dp_object *d,int index)
{
    lan_peer *p=&d->peers[index];
    for(int pass=0;pass<8;pass++) {
        uint8_t bytes[16384];ssize_t n=recv(p->fd,bytes,sizeof bytes,0);
        if(n==0)return 0;if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK)break;if(errno==EINTR)continue;return 0;}
        if(p->used+(size_t)n>LAN_MAX_DATA+20+sizeof bytes)return 0;
        if(p->used+(size_t)n>p->capacity){size_t cap=p->used+(size_t)n;uint8_t *b=realloc(p->input,cap);if(!b)return 0;p->input=b;p->capacity=cap;}
        memcpy(p->input+p->used,bytes,(size_t)n);p->used+=(size_t)n;
        size_t consumed=0;
        while(p->used-consumed>=20) {
            uint32_t h[5];memcpy(h,p->input+consumed,20);for(int k=0;k<5;k++)h[k]=ntohl(h[k]);
            if(h[0]!=LAN_MAGIC||h[4]>LAN_MAX_DATA)return 0;if(p->used-consumed<20+h[4])break;
            if(!process_packet(d,index,h[1],h[2],h[3],p->input+consumed+20,h[4]))return 0;consumed+=20+h[4];
        }
        if(consumed){p->used-=consumed;memmove(p->input,p->input+consumed,p->used);}
    }
    for(int pass=0;pass<8&&p->first;pass++) {
        lan_packet *q=p->first;ssize_t n=send(p->fd,q->bytes+q->sent,q->size-q->sent,MSG_NOSIGNAL);
        if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK)break;if(errno==EINTR)continue;return 0;}if(!n)return 0;
        q->sent+=(size_t)n;if(q->sent==q->size){p->queued-=q->size;p->first=q->next;if(!p->first)p->last=NULL;free(q);}
    }
    return 1;
}
static void *network_worker(void *arg)
{
    dp_object *d=arg;
    for(;;) {
        pthread_mutex_lock(&d->lock);if(d->stop){pthread_mutex_unlock(&d->lock);break;}
        if(d->hosting) {
            for(int k=0;k<LAN_PLAYERS;k++) {
                int fd=accept(d->listener,NULL,NULL);if(fd<0)break;
                int slot=-1;for(int i=0;i<LAN_PLAYERS;i++)if(d->peers[i].fd<0){slot=i;break;}
                if(slot<0||!nonblocking(fd)){close(fd);continue;}
                int one=1;setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
                d->peers[slot].fd=fd;d->peers[slot].id=d->next_id++;
                d->peers[slot].accepted=void_monotonic_ns();
            }
            for(int k=0;k<16;k++) {
                struct sockaddr_in address;socklen_t length=sizeof address;uint32_t query[9];
                ssize_t n=recvfrom(d->discovery,query,sizeof query,0,(struct sockaddr *)&address,&length);if(n<0)break;
                if(n==sizeof query&&ntohl(query[0])==LAN_MAGIC&&!memcmp(query+1,d->session.desc+6,16)&&!(d->session.desc[1]&0x21)) {
                    uint8_t answer[4+sizeof(lan_session)];uint32_t magic=htonl(LAN_MAGIC);memcpy(answer,&magic,4);memcpy(answer+4,&d->session,sizeof d->session);
                    sendto(d->discovery,answer,sizeof answer,MSG_NOSIGNAL,(struct sockaddr *)&address,length);
                }
            }
        }
        for(int i=0;i<LAN_PLAYERS;i++)if(d->peers[i].fd>=0) {
            lan_peer *p=&d->peers[i];
            if((d->hosting&&!p->welcomed&&void_monotonic_ns()-p->accepted>5000000000ull)||!peer_io(d,i))drop_peer(d,i);
        }
        if(d->lost&&!d->loss_reported){d->loss_reported=1;atomic_store(&disconnected,1);void_log("[lan] connection lost");}
        pthread_mutex_unlock(&d->lock);poll(NULL,0,5);
    }
    return NULL;
}
static void close_session(dp_object *d)
{
    pthread_mutex_lock(&d->lock);d->stop=1;pthread_mutex_unlock(&d->lock);
    if(d->worker_started)pthread_join(d->worker,NULL);d->worker_started=0;
    pthread_mutex_lock(&d->lock);d->open=0;
    for(int i=0;i<LAN_PLAYERS;i++)drop_peer(d,i);
    if(d->listener>=0)close(d->listener);if(d->discovery>=0)close(d->discovery);d->listener=d->discovery=-1;
    while(d->first){lan_message *m=d->first;d->first=m->next;free(m);}d->last=NULL;d->queued=d->messages=0;
    memset(d->players,0,sizeof d->players);memset(d->local_lengths,0,sizeof d->local_lengths);
    d->lost=d->welcomed=d->loss_reported=0;pthread_mutex_unlock(&d->lock);
}
static void destroy_dp(void_com_object *object)
{
    dp_object *d=(dp_object *)object;close_session(d);pthread_mutex_destroy(&d->lock);pthread_cond_destroy(&d->ready);
    if(!d->lobby)atomic_fetch_sub(&dp_objects,1);free(d);
}

static int iid_supported(dp_object *d,uint32_t iid)
{
    if(!readable(iid,16))return 0;uint32_t a=MEM32(iid);
    if(a==0&&MEM32(iid+4)==0&&MEM32(iid+8)==0xC0&&MEM32(iid+12)==0x46000000)return 1;
    static const uint32_t ids[][4]={
        {0x2DB72490,0x11D1652C,0x0000A8A7,0xFCAB03F8},{0x2DB72491,0x11D1652C,0x0000A8A7,0xFCAB03F8},
        {0x0194C220,0x11D0A303,0xA0004F9C,0x5E4205C9},{0x1BB4AF80,0x11D0A303,0xA0004F9C,0x5E4205C9},
        {0x0AB1C530,0x11D14745,0x0000A1A7,0xFCAB03F8},{0x0AB1C531,0x11D14745,0x0000A1A7,0xFCAB03F8},
        {0x133EFE40,0x11D032DC,0xA000FB9C,0xCB430AC9},{0x133EFE41,0x11D032DC,0xA000FB9C,0xCB430AC9},
        {0x2B74F7C0,0x11CF9154,0xAA00CDA9,0xE3866800},{0x9D460580,0x11CFA822,0x80000C96,0x824E53C7}};
    for(unsigned i=d->lobby?0:4;i<(d->lobby?4:10);i++)if(!memcmp(GPTR(iid),ids[i],16))return 1;
    return 0;
}
COM_METHOD(dp,QueryInterface,3)
{
    dp_object *d=void_com_host(THIS_);if(!writable(ARG(2),4))return E_POINTER_;
    MEM32(ARG(2))=0;if(!iid_supported(d,ARG(1)))return 0x80004002;
    uint32_t iid=MEM32(ARG(1));int ansi=iid==0x2DB72491||iid==0x1BB4AF80||iid==0x0AB1C531||iid==0x133EFE41||iid==0x9D460580;
    if(iid&&ansi==d->wide)return 0x80004002;
    MEM32(ARG(2))=THIS_;void_com_addref(&d->com);return 0;
}
COM_METHOD(dp,AddRef,1){return void_com_addref(void_com_host(THIS_));}
COM_METHOD(dp,Release,1){return void_com_release(void_com_host(THIS_));}
COM_METHOD(dp,Close,1){close_session(void_com_host(THIS_));return 0;}
static uint32_t buffer_result(uint32_t out,uint32_t length,uint32_t needed)
{
    if(!writable(length,4))return DP_INVALID;uint32_t have=MEM32(length);MEM32(length)=needed;
    if(!out||have<needed)return DP_BUFFER_SMALL;
    return writable(out,needed)?0:DP_INVALID;
}
static uint32_t write_session(dp_object *d,const lan_session *s,uint32_t out,uint32_t length)
{
    uint32_t needed=80+(uint32_t)name_size(s->name,d->wide),result=buffer_result(out,length,needed);if(result)return result;
    memcpy(GPTR(out),s->desc,80);MEM32(out+48)=out+80;MEM32(out+52)=0;put_name(out+80,s->name,d->wide);return 0;
}
COM_METHOD(dp,GetSessionDesc,3)
{
    dp_object *d=void_com_host(THIS_);pthread_mutex_lock(&d->lock);lan_session s=d->session;int open=d->open;pthread_mutex_unlock(&d->lock);
    return open?write_session(d,&s,ARG(1),ARG(2)):DP_NO_CONNECTION;
}
static int read_session(dp_object *d,uint32_t p,lan_session *s)
{
    if(!readable(p,80)||MEM32(p)!=80)return 0;memset(s,0,sizeof *s);memcpy(s->desc,GPTR(p),80);
    get_name(MEM32(p+48),d->wide,s->name);s->desc[12]=s->desc[13]=0;
    /* The port has no password negotiation; a protected session cannot become public. */
    if(MEM32(p+52)&&readable(MEM32(p+52),2)&&MEM16(MEM32(p+52)))return 0;
    if(!s->desc[10])s->desc[10]=LAN_PLAYERS;return s->desc[10]<=LAN_PLAYERS;
}
COM_METHOD(dp,EnumConnections,5)
{
    dp_object *d=void_com_host(THIS_);if(!ARG(2))return DP_INVALID;
    uint32_t p=void_galloc(512);if(!p)return E_OUTOFMEMORY_;
    memcpy(GPTR(p),guid_tcp,16);memcpy(GPTR(p+16),guid_sp,16);MEM32(p+32)=16;memcpy(GPTR(p+36),guid_tcp,16);
    MEM32(p+64)=16;MEM32(p+68)=0;MEM32(p+72)=p+80;MEM32(p+76)=p+80;
    const uint16_t name[]={'T','C','P','/','I','P',' ','L','A','N',0};put_name(p+80,name,d->wide);
    uint32_t args[]={p,p+16,36,p+64,1,ARG(3)};void_call_guest(ARG(2),6,args,1);void_gfree(p);return 0;
}
COM_METHOD(dp,Initialize,2){return 0;}
COM_METHOD(dp,InitializeConnection,3)
{
    dp_object *d=void_com_host(THIS_);uint32_t p=ARG(1);d->address[0]=0;
    unsigned total=36;
    if(!readable(p,20))return DP_INVALID;
    if(!memcmp(GPTR(p),guid_size,16)) {
        if(MEM32(p+16)!=4||!readable(p+20,4))return DP_INVALID;
        total=MEM32(p+20);if(total<24||total>4096)return DP_INVALID;
    } else if(memcmp(GPTR(p),guid_sp,16))return DP_INVALID;
    for(unsigned offset=0;offset<total;) {
        if(total-offset<20||!readable(p+offset,20))return DP_INVALID;
        uint32_t n=MEM32(p+offset+16);if(n>total-offset-20||(n&&!readable(p+offset+20,n)))return DP_INVALID;
        if(!memcmp(GPTR(p+offset),guid_inet,16)&&n&&n<sizeof d->address){
            memcpy(d->address,GPTR(p+offset+20),n);d->address[n]=0;
            struct in_addr ip;if(d->address[0]&&inet_pton(AF_INET,d->address,&ip)!=1)return DP_INVALID;
        }
        offset+=20+n;
    }
    d->initialized=1;void_log("[lan] TCP/IP initialized, target %s",d->address[0]?d->address:"broadcast");return 0;
}
static void discover(dp_object *d,const uint8_t app[16])
{
    int fd=make_socket(SOCK_DGRAM,0);if(fd<0)return;
    uint32_t query[9]={htonl(LAN_MAGIC)};memcpy(query+1,app,16);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(LAN_DISCOVERY),.sin_addr.s_addr=INADDR_BROADCAST};
    sendto(fd,query,sizeof query,0,(struct sockaddr *)&a,sizeof a);
    struct ifaddrs *addresses=NULL;
    if(!getifaddrs(&addresses))for(struct ifaddrs *i=addresses;i;i=i->ifa_next)if(i->ifa_addr&&i->ifa_addr->sa_family==AF_INET&&i->ifa_broadaddr) {
        a.sin_addr=((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr;sendto(fd,query,sizeof query,0,(struct sockaddr *)&a,sizeof a);
    }
    if(addresses)freeifaddrs(addresses);
    if(d->address[0]){inet_pton(AF_INET,d->address,&a.sin_addr);sendto(fd,query,sizeof query,0,(struct sockaddr *)&a,sizeof a);}
    d->found_count=0;uint64_t end=void_monotonic_ns()+300000000u;
    while(void_monotonic_ns()<end) {
        struct pollfd wait={.fd=fd,.events=POLLIN};if(poll(&wait,1,30)<=0)continue;
        uint8_t answer[4+sizeof(lan_session)];struct sockaddr_in source;socklen_t length=sizeof source;
        ssize_t n=recvfrom(fd,answer,sizeof answer,0,(struct sockaddr *)&source,&length);
        if(n!=sizeof answer)continue;
        uint32_t magic=0;memcpy(&magic,answer,4);lan_session session;
        if(ntohl(magic)!=LAN_MAGIC)continue;memcpy(&session,answer+4,sizeof session);
        if(!valid_session(&session)||memcmp(session.desc+6,app,16))continue;
        unsigned i=0;for(;i<d->found_count;i++)if(!memcmp(d->found[i].session.desc+2,session.desc+2,16))break;
        if(i==d->found_count&&i<32){d->found[i].session=session;d->found[i].ip=source.sin_addr;d->found_count++;}
    }
    close(fd);
}
COM_METHOD(dp,EnumSessions,6)
{
    dp_object *d=void_com_host(THIS_);if(!ARG(3)||!readable(ARG(1),80))return DP_INVALID;
    if(ARG(5)&0x60)return 0; /* STOPASYNC, or a request for password-protected sessions. */
    uint32_t callback=ARG(3),user=ARG(4);uint8_t app[16];memcpy(app,GPTR(ARG(1)+24),16);
    void_blocking_begin();discover(d,app);void_blocking_end();
    uint32_t p=void_galloc(1024);if(!p)return E_OUTOFMEMORY_;
    for(unsigned i=0;i<d->found_count;i++) {
        MEM32(p+1000)=1000;write_session(d,&d->found[i].session,p,p+1000);
        uint32_t args[]={p,p+1004,0,user};MEM32(p+1004)=0;if(!void_call_guest(callback,4,args,1))break;
    }
    uint32_t args[]={0,p+1004,1,user};void_call_guest(callback,4,args,1);void_gfree(p);return 0;
}
COM_METHOD(dp,Open,3)
{
    dp_object *d=void_com_host(THIS_);lan_session s;
    if(!d->initialized||!read_session(d,ARG(1),&s))return DP_INVALID;
    uint32_t flags=ARG(2);if(!(flags&3)||flags&~3u)return DP_INVALID;
    close_session(d);d->stop=0;d->hosting=!!(flags&2);d->session=s;d->next_id=2;
    if(d->hosting) {
        d->listener=make_socket(SOCK_STREAM,LAN_PORT);d->discovery=make_socket(SOCK_DGRAM,LAN_DISCOVERY);
        if(d->listener<0||d->discovery<0||listen(d->listener,8)){close_session(d);return DP_NO_CONNECTION;}
        for(int i=0;i<4;i++)d->session.desc[i+2]=(uint32_t)rand()^((uint32_t)void_monotonic_ns()>>(i*5));
        d->session.desc[11]=0;d->local_id=1;d->welcomed=1;
    } else {
        struct in_addr ip={0};
        for(unsigned i=0;i<d->found_count;i++)if(!memcmp(d->found[i].session.desc+2,s.desc+2,16)){ip=d->found[i].ip;break;}
        if(!ip.s_addr&&d->address[0])inet_pton(AF_INET,d->address,&ip);
        if(!ip.s_addr)return DP_NO_CONNECTION;
        int fd=make_socket(SOCK_STREAM,0);if(fd<0)return DP_NO_CONNECTION;
        struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(LAN_PORT),.sin_addr=ip};
        void_blocking_begin();int ok=connect(fd,(struct sockaddr *)&a,sizeof a)==0;
        if(!ok&&errno==EINPROGRESS){struct pollfd wait={.fd=fd,.events=POLLOUT};int error=1;socklen_t n=sizeof error;
            ok=poll(&wait,1,5000)>0&&getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&n)==0&&!error;}
        void_blocking_end();if(!ok){close(fd);return DP_NO_CONNECTION;}
        d->peers[0].fd=fd;d->peers[0].id=1;
        if(!queue_packet(&d->peers[0],NET_HELLO,0,0,s.desc+2,32)){close_session(d);return E_OUTOFMEMORY_;}
    }
    d->open=1;
    if(pthread_create(&d->worker,NULL,network_worker,d)){close_session(d);return DP_NO_CONNECTION;}d->worker_started=1;
    if(!d->hosting) {
        struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=5;
        void_blocking_begin();pthread_mutex_lock(&d->lock);
        while(!d->welcomed&&!d->lost)if(pthread_cond_timedwait(&d->ready,&d->lock,&deadline))break;
        int ok=d->welcomed;pthread_mutex_unlock(&d->lock);void_blocking_end();
        if(!ok){close_session(d);return DP_NO_CONNECTION;}
    }
    void_log("[lan] %s session ready, player %u, TCP %d",d->hosting?"host":"join",d->local_id,LAN_PORT);return 0;
}
COM_METHOD(dp,CreatePlayer,7)
{
    dp_object *d=void_com_host(THIS_);if(!writable(ARG(1),4)||!readable(ARG(2),16)||ARG(5)>LAN_PLAYER_DATA||
        (ARG(5)&&!readable(ARG(4),ARG(5))))return DP_INVALID;
    lan_player player={.id=d->local_id,.flags=ARG(6),.length=ARG(5)};
    uint32_t name=MEM32(ARG(2)+8);if(!name)name=MEM32(ARG(2)+12);get_name(name,d->wide,player.name);
    if(player.length)memcpy(player.data,GPTR(ARG(4)),player.length);
    pthread_mutex_lock(&d->lock);int n=-1;
    for(int i=0;i<LAN_PLAYERS;i++)if(!d->players[i].id){n=i;break;}
    if(!d->open||n<0||!player.id){pthread_mutex_unlock(&d->lock);return DP_NO_CONNECTION;}
    d->players[n]=player;refresh_session(d);
    if(d->hosting)relay(d,-1,NET_PLAYER,d->local_id,0,&player,sizeof player);
    else if(!queue_packet(&d->peers[0],NET_PLAYER,d->local_id,0,&player,sizeof player)){pthread_mutex_unlock(&d->lock);return DP_NO_CONNECTION;}
    for(int i=0;i<LAN_PLAYERS;i++)if(d->players[i].id&&d->players[i].id!=d->local_id)queue_message(d,3,0,d->local_id,&d->players[i],sizeof(lan_player));
    MEM32(ARG(1))=d->local_id;pthread_mutex_unlock(&d->lock);return 0;
}
COM_METHOD(dp,EnumPlayers,5)
{
    dp_object *d=void_com_host(THIS_);if(!ARG(2))return DP_INVALID;uint32_t callback=ARG(2),user=ARG(3);
    lan_player players[LAN_PLAYERS];pthread_mutex_lock(&d->lock);memcpy(players,d->players,sizeof players);pthread_mutex_unlock(&d->lock);
    uint32_t p=void_galloc(512);if(!p)return E_OUTOFMEMORY_;
    for(int i=0;i<LAN_PLAYERS;i++)if(players[i].id) {
        MEM32(p)=16;MEM32(p+4)=0;MEM32(p+8)=MEM32(p+12)=p+16;put_name(p+16,players[i].name,d->wide);
        uint32_t flags=(players[i].id==d->local_id?8:16)|(players[i].id==1?0x100:0);
        uint32_t args[]={players[i].id,1,p,flags,user};if(!void_call_guest(callback,5,args,1))break;
    }
    void_gfree(p);return 0;
}
COM_METHOD(dp,GetPlayerName,4)
{
    dp_object *d=void_com_host(THIS_);pthread_mutex_lock(&d->lock);int n=player_index(d,ARG(1));
    if(n<0){pthread_mutex_unlock(&d->lock);return DP_INVALID_PLAYER;}
    lan_player p=d->players[n];pthread_mutex_unlock(&d->lock);
    uint32_t out=ARG(2),needed=16+(uint32_t)name_size(p.name,d->wide),r=buffer_result(out,ARG(3),needed);if(r)return r;
    MEM32(out)=16;MEM32(out+4)=0;MEM32(out+8)=MEM32(out+12)=out+16;put_name(out+16,p.name,d->wide);return 0;
}
COM_METHOD(dp,GetPlayerData,5)
{
    dp_object *d=void_com_host(THIS_);pthread_mutex_lock(&d->lock);int n=player_index(d,ARG(1));uint32_t r=DP_INVALID_PLAYER;
    if(n>=0) {uint32_t length=(ARG(4)&1)?d->local_lengths[n]:d->players[n].length;
        r=buffer_result(ARG(2),ARG(3),length);if(!r&&length)memcpy(GPTR(ARG(2)),(ARG(4)&1)?d->local_data[n]:d->players[n].data,length);}
    pthread_mutex_unlock(&d->lock);return r;
}
COM_METHOD(dp,SetPlayerData,5)
{
    dp_object *d=void_com_host(THIS_);uint32_t size=ARG(3);if(size>LAN_PLAYER_DATA||(size&&!readable(ARG(2),size)))return DP_INVALID;
    pthread_mutex_lock(&d->lock);int n=player_index(d,ARG(1));if(n<0){pthread_mutex_unlock(&d->lock);return DP_INVALID_PLAYER;}
    if(ARG(4)&1){d->local_lengths[n]=size;if(size)memcpy(d->local_data[n],GPTR(ARG(2)),size);}
    else {
        if(ARG(1)!=d->local_id){pthread_mutex_unlock(&d->lock);return DP_INVALID_PLAYER;}
        d->players[n].length=size;if(size)memcpy(d->players[n].data,GPTR(ARG(2)),size);
        if(d->hosting)relay(d,-1,NET_PLAYER_DATA,d->local_id,0,&d->players[n],sizeof(lan_player));
        else if(!queue_packet(&d->peers[0],NET_PLAYER_DATA,d->local_id,0,&d->players[n],sizeof(lan_player)))d->lost=1;
    }
    int lost=d->lost;pthread_mutex_unlock(&d->lock);return lost?DP_NO_CONNECTION:0;
}
COM_METHOD(dp,SetSessionDesc,3)
{
    dp_object *d=void_com_host(THIS_);lan_session s;if(!read_session(d,ARG(1),&s))return DP_INVALID;
    pthread_mutex_lock(&d->lock);if(!d->hosting||!d->open){pthread_mutex_unlock(&d->lock);return DP_NO_CONNECTION;}
    memcpy(s.desc+2,d->session.desc+2,32);d->session=s;refresh_session(d);relay(d,-1,NET_SESSION,0,0,&d->session,sizeof s);
    pthread_mutex_unlock(&d->lock);return 0;
}
COM_METHOD(dp,Send,6)
{
    dp_object *d=void_com_host(THIS_);uint32_t from=ARG(1),to=ARG(2),size=ARG(5);if(size>LAN_MAX_DATA||(size&&!readable(ARG(4),size)))return DP_INVALID;
    pthread_mutex_lock(&d->lock);
    if(!d->open||d->lost||from!=d->local_id||player_index(d,from)<0){pthread_mutex_unlock(&d->lock);return DP_NO_CONNECTION;}
    if(to&&player_index(d,to)<0){pthread_mutex_unlock(&d->lock);return DP_INVALID_PLAYER;}
    if(to==from)queue_message(d,0,from,to,GPTR(ARG(4)),size);
    else if(d->hosting)relay(d,-1,NET_DATA,from,to,GPTR(ARG(4)),size);
    else if(!queue_packet(&d->peers[0],NET_DATA,from,to,GPTR(ARG(4)),size))d->lost=1;
    int lost=d->lost;pthread_mutex_unlock(&d->lock);return lost?DP_NO_CONNECTION:0;
}
static uint32_t message_size(dp_object *d,const lan_message *m)
{
    if(m->kind==3){const lan_player *p=(const lan_player *)m->bytes;return 48+(uint32_t)name_size(p->name,d->wide)+p->length;}
    if(m->kind==5)return 52;
    if(m->kind==0x102){const lan_player *p=(const lan_player *)m->bytes;return 20+p->length;}
    if(m->kind==0x104){const lan_session *s=(const lan_session *)m->bytes;return 84+(uint32_t)name_size(s->name,d->wide);}
    if(m->kind)return 4;
    return m->size;
}
static void write_message(dp_object *d,const lan_message *m,uint32_t out,uint32_t size)
{
    if(!m->kind){if(size)memcpy(GPTR(out),m->bytes,size);return;}
    memset(GPTR(out),0,size);MEM32(out)=m->kind;
    if(m->kind==3) {
        const lan_player *p=(const lan_player *)m->bytes;uint32_t names=(uint32_t)name_size(p->name,d->wide);
        MEM32(out+4)=1;MEM32(out+8)=p->id;MEM32(out+12)=player_count(d);MEM32(out+16)=p->length?out+48+names:0;MEM32(out+20)=p->length;
        MEM32(out+24)=16;MEM32(out+32)=MEM32(out+36)=out+48;MEM32(out+44)=p->id==1?0x100:0;
        put_name(out+48,p->name,d->wide);if(p->length)memcpy(GPTR(out+48+names),p->data,p->length);
    } else if(m->kind==5){MEM32(out+4)=1;memcpy(GPTR(out+8),m->bytes,4);MEM32(out+28)=16;}
    else if(m->kind==0x102){const lan_player *p=(const lan_player *)m->bytes;MEM32(out+4)=1;MEM32(out+8)=p->id;
        MEM32(out+12)=p->length?out+20:0;MEM32(out+16)=p->length;if(p->length)memcpy(GPTR(out+20),p->data,p->length);}
    else if(m->kind==0x104){const lan_session *s=(const lan_session *)m->bytes;memcpy(GPTR(out+4),s->desc,80);MEM32(out+52)=out+84;put_name(out+84,s->name,d->wide);}
}
COM_METHOD(dp,Receive,6)
{
    dp_object *d=void_com_host(THIS_);if(!writable(ARG(1),4)||!writable(ARG(2),4)||!writable(ARG(5),4))return DP_INVALID;
    pthread_mutex_lock(&d->lock);lan_message *m=d->first,*previous=NULL;uint32_t flags=ARG(3);
    while(m&&(((flags&2)&&m->to!=MEM32(ARG(2)))||((flags&4)&&m->from!=MEM32(ARG(1))))){previous=m;m=m->next;}
    if(!m){pthread_mutex_unlock(&d->lock);return DP_NO_MESSAGES;}
    uint32_t size=message_size(d,m),r=buffer_result(ARG(4),ARG(5),size);
    if(!r) {
        write_message(d,m,ARG(4),size);MEM32(ARG(1))=m->from;MEM32(ARG(2))=m->to;
        if(!(flags&8)){if(previous)previous->next=m->next;else d->first=m->next;if(d->last==m)d->last=previous;
            d->messages--;d->queued-=m->size+sizeof *m;free(m);}
    }
    pthread_mutex_unlock(&d->lock);return r;
}
COM_METHOD(dp,GetMessageCount,3)
{
    dp_object *d=void_com_host(THIS_);if(!writable(ARG(2),4))return DP_INVALID;pthread_mutex_lock(&d->lock);
    uint32_t n=0;for(lan_message *m=d->first;m;m=m->next)if(!ARG(1)||ARG(1)==m->to)n++;MEM32(ARG(2))=n;pthread_mutex_unlock(&d->lock);return 0;
}
static uint32_t caps(dp_object *d,uint32_t p)
{
    if(!writable(p,40)||MEM32(p)!=40)return DP_INVALID;uint32_t values[]={40,64|(d->hosting?2u:0u),LAN_MAX_DATA,LAN_QUEUE_BYTES,8,1000000,10,1,20,5000};
    memcpy(GPTR(p),values,sizeof values);return 0;
}
COM_METHOD(dp,GetCaps,3){return caps(void_com_host(THIS_),ARG(1));}
COM_METHOD(dp,GetPlayerCaps,4){return caps(void_com_host(THIS_),ARG(2));}
COM_METHOD(dp,GetPlayerFlags,3)
{
    dp_object *d=void_com_host(THIS_);if(!writable(ARG(2),4))return DP_INVALID;
    pthread_mutex_lock(&d->lock);int n=player_index(d,ARG(1));
    if(n>=0)MEM32(ARG(2))=(ARG(1)==d->local_id?8:16)|(ARG(1)==1?0x100:0);pthread_mutex_unlock(&d->lock);return n>=0?0:DP_INVALID_PLAYER;
}
COM_METHOD(dp,DestroyPlayer,2)
{
    dp_object *d=void_com_host(THIS_);pthread_mutex_lock(&d->lock);int n=player_index(d,ARG(1));
    if(n>=0&&ARG(1)==d->local_id)memset(&d->players[n],0,sizeof(lan_player));refresh_session(d);
    pthread_mutex_unlock(&d->lock);return n>=0?0:DP_INVALID_PLAYER;
}

void stronghold_lan_address(char ip[64])
{
    strcpy(ip,"127.0.0.1");struct ifaddrs *addresses=NULL;if(getifaddrs(&addresses))return;
    for(struct ifaddrs *i=addresses;i;i=i->ifa_next)if(i->ifa_addr&&i->ifa_addr->sa_family==AF_INET) {
        struct in_addr a=((struct sockaddr_in *)i->ifa_addr)->sin_addr;if((ntohl(a.s_addr)>>24)==127)continue;
        inet_ntop(AF_INET,&a,ip,64);if(!strncmp(i->ifa_name,"wlan",4))break;
    }
    freeifaddrs(addresses);
}
COM_METHOD(dp,GetPlayerAddress,4)
{
    char ip[64];stronghold_lan_address(ip);uint32_t size=(uint32_t)strlen(ip)+2,needed=20+size;
    uint32_t r=buffer_result(ARG(2),ARG(3),needed);if(r)return r;
    memcpy(GPTR(ARG(2)),guid_inet,16);MEM32(ARG(2)+16)=size;memset(GPTR(ARG(2)+20),0,size);memcpy(GPTR(ARG(2)+20),ip,size-2);return 0;
}
COM_METHOD(dp,SetPlayerName,4)
{
    dp_object *d=void_com_host(THIS_);if(!readable(ARG(2),16))return DP_INVALID;uint16_t name[LAN_NAME];get_name(MEM32(ARG(2)+8),d->wide,name);
    pthread_mutex_lock(&d->lock);int n=player_index(d,ARG(1));if(n<0||ARG(1)!=d->local_id){pthread_mutex_unlock(&d->lock);return DP_INVALID_PLAYER;}
    memcpy(d->players[n].name,name,sizeof name);
    if(d->hosting)relay(d,-1,NET_PLAYER,d->local_id,0,&d->players[n],sizeof(lan_player));
    else if(!queue_packet(&d->peers[0],NET_PLAYER,d->local_id,0,&d->players[n],sizeof(lan_player)))d->lost=1;
    pthread_mutex_unlock(&d->lock);return 0;
}
COM_METHOD(dp,SendEx,10)
{
    /* SendEx's leading arguments match Send; this port completes writes in its worker. */
    uint32_t r=dp_Send_impl();if(writable(ARG(9),4))MEM32(ARG(9))=0;return r;
}
COM_METHOD(dp,GetMessageQueue,6)
{
    dp_object *d=void_com_host(THIS_);pthread_mutex_lock(&d->lock);uint32_t messages=0,bytes=0;
    for(lan_message *m=d->first;m;m=m->next)if((!ARG(1)||m->from==ARG(1))&&(!ARG(2)||m->to==ARG(2))) {
        messages++;bytes+=message_size(d,m);
    }
    if(writable(ARG(4),4))MEM32(ARG(4))=messages;if(writable(ARG(5),4))MEM32(ARG(5))=bytes;pthread_mutex_unlock(&d->lock);return 0;
}
COM_METHOD(dp,CancelMessage,3){return 0;}
COM_METHOD(dp,CancelPriority,4){return 0;}

#define DP_UNAVAILABLE(name,n) COM_METHOD(dp,name,n){return DP_UNSUPPORTED;}
DP_UNAVAILABLE(AddPlayerToGroup,3)
DP_UNAVAILABLE(CreateGroup,6)
DP_UNAVAILABLE(DeletePlayerFromGroup,3)
DP_UNAVAILABLE(DestroyGroup,2)
DP_UNAVAILABLE(EnumGroupPlayers,6)
DP_UNAVAILABLE(EnumGroups,5)
DP_UNAVAILABLE(GetGroupData,5)
DP_UNAVAILABLE(GetGroupName,4)
DP_UNAVAILABLE(SetGroupData,5)
DP_UNAVAILABLE(SetGroupName,4)
DP_UNAVAILABLE(AddGroupToGroup,3)
DP_UNAVAILABLE(CreateGroupInGroup,7)
DP_UNAVAILABLE(DeleteGroupFromGroup,3)
DP_UNAVAILABLE(EnumGroupsInGroup,6)
DP_UNAVAILABLE(GetGroupConnectionSettings,5)
DP_UNAVAILABLE(SecureOpen,5)
DP_UNAVAILABLE(SendChatMessage,5)
DP_UNAVAILABLE(SetGroupConnectionSettings,4)
DP_UNAVAILABLE(StartSession,3)
DP_UNAVAILABLE(GetGroupFlags,3)
DP_UNAVAILABLE(GetGroupParent,3)
DP_UNAVAILABLE(GetPlayerAccount,5)
DP_UNAVAILABLE(GetGroupOwner,3)
DP_UNAVAILABLE(SetGroupOwner,3)
static const void_fn dp_methods[]={
    dp_QueryInterface,dp_AddRef,dp_Release,dp_AddPlayerToGroup,dp_Close,dp_CreateGroup,dp_CreatePlayer,
    dp_DeletePlayerFromGroup,dp_DestroyGroup,dp_DestroyPlayer,dp_EnumGroupPlayers,dp_EnumGroups,dp_EnumPlayers,
    dp_EnumSessions,dp_GetCaps,dp_GetGroupData,dp_GetGroupName,dp_GetMessageCount,dp_GetPlayerAddress,dp_GetPlayerCaps,
    dp_GetPlayerData,dp_GetPlayerName,dp_GetSessionDesc,dp_Initialize,dp_Open,dp_Receive,dp_Send,dp_SetGroupData,
    dp_SetGroupName,dp_SetPlayerData,dp_SetPlayerName,dp_SetSessionDesc,dp_AddGroupToGroup,dp_CreateGroupInGroup,
    dp_DeleteGroupFromGroup,dp_EnumConnections,dp_EnumGroupsInGroup,dp_GetGroupConnectionSettings,dp_InitializeConnection,
    dp_SecureOpen,dp_SendChatMessage,dp_SetGroupConnectionSettings,dp_StartSession,dp_GetGroupFlags,dp_GetGroupParent,
    dp_GetPlayerAccount,dp_GetPlayerFlags,dp_GetGroupOwner,dp_SetGroupOwner,dp_SendEx,dp_GetMessageQueue,dp_CancelMessage,dp_CancelPriority
};
static void_com_class dp_class={"DirectPlay4",53,dp_methods,0};

COM_METHOD(lobby,CreateCompoundAddress,5)
{
    uint32_t elements=ARG(1),count=ARG(2),total=24;if(count>32||!readable(elements,count*24))return DP_INVALID;
    for(uint32_t i=0;i<count;i++){uint32_t n=MEM32(elements+i*24+16),p=MEM32(elements+i*24+20);
        if(n>4096||total+n+20>4096||(n&&!readable(p,n)))return DP_INVALID;total+=n+20;}
    uint32_t r=buffer_result(ARG(3),ARG(4),total);if(r)return r;
    memcpy(GPTR(ARG(3)),guid_size,16);MEM32(ARG(3)+16)=4;MEM32(ARG(3)+20)=total;
    uint32_t offset=24;for(uint32_t i=0;i<count;i++){uint32_t n=MEM32(elements+i*24+16),p=MEM32(elements+i*24+20);
        memcpy(GPTR(ARG(3)+offset),GPTR(elements+i*24),20);if(n)memcpy(GPTR(ARG(3)+offset+20),GPTR(p),n);offset+=n+20;}
    return 0;
}
COM_METHOD(lobby,EnumAddress,5)
{
    uint32_t callback=ARG(1),p=ARG(2),size=ARG(3),user=ARG(4);if(!callback||size>4096||!readable(p,size))return DP_INVALID;
    for(uint32_t offset=0;offset<size;) {
        if(size-offset<20)return DP_INVALID;uint32_t n=MEM32(p+offset+16);if(n>size-offset-20)return DP_INVALID;
        uint32_t args[]={p+offset,n,p+offset+20,user};if(!void_call_guest(callback,4,args,1))break;offset+=20+n;
    }
    return 0;
}
#define LOBBY_UNAVAILABLE(name,n) COM_METHOD(lobby,name,n){return DP_UNSUPPORTED;}
LOBBY_UNAVAILABLE(Connect,4)
LOBBY_UNAVAILABLE(CreateAddress,7)
LOBBY_UNAVAILABLE(EnumAddressTypes,5)
LOBBY_UNAVAILABLE(EnumLocalApplications,4)
LOBBY_UNAVAILABLE(GetConnectionSettings,4)
LOBBY_UNAVAILABLE(ReceiveLobbyMessage,6)
LOBBY_UNAVAILABLE(RunApplication,5)
LOBBY_UNAVAILABLE(SendLobbyMessage,5)
LOBBY_UNAVAILABLE(SetConnectionSettings,4)
LOBBY_UNAVAILABLE(SetLobbyMessageEvent,4)
LOBBY_UNAVAILABLE(ConnectEx,5)
LOBBY_UNAVAILABLE(RegisterApplication,3)
LOBBY_UNAVAILABLE(UnregisterApplication,3)
LOBBY_UNAVAILABLE(WaitForConnectionSettings,2)
static const void_fn lobby_methods[]={dp_QueryInterface,dp_AddRef,dp_Release,lobby_Connect,lobby_CreateAddress,lobby_EnumAddress,
    lobby_EnumAddressTypes,lobby_EnumLocalApplications,lobby_GetConnectionSettings,lobby_ReceiveLobbyMessage,lobby_RunApplication,
    lobby_SendLobbyMessage,lobby_SetConnectionSettings,lobby_SetLobbyMessageEvent,lobby_CreateCompoundAddress,lobby_ConnectEx,
    lobby_RegisterApplication,lobby_UnregisterApplication,lobby_WaitForConnectionSettings};
static void_com_class lobby_class={"DirectPlayLobby3",19,lobby_methods,0};
static dp_object *new_dp(int lobby,int wide)
{
    dp_object *d=calloc(1,sizeof *d);if(!d)return NULL;
    d->lobby=lobby;d->wide=wide;d->listener=d->discovery=-1;for(int i=0;i<LAN_PLAYERS;i++)d->peers[i].fd=-1;
    pthread_mutex_init(&d->lock,NULL);pthread_cond_init(&d->ready,NULL);d->com.destroy=destroy_dp;
    if(!lobby)atomic_fetch_add(&dp_objects,1);
    void_com_wrap(&d->com,lobby?&lobby_class:&dp_class,0);return d;
}
uint32_t stronghold_dplay_create(uint32_t clsid,uint32_t iid,uint32_t out)
{
    if(!readable(clsid,16))return DP_INVALID;
    int lobby=!memcmp(GPTR(clsid),guid_lobby,16);if(!lobby&&memcmp(GPTR(clsid),guid_dp,16))return 0x80040154;
    if(!writable(out,4)||!readable(iid,16))return E_POINTER_;MEM32(out)=0;
    dp_object probe={.lobby=lobby};if(!iid_supported(&probe,iid))return 0x80004002;
    uint32_t a=MEM32(iid);int wide=a!=0x0AB1C531&&a!=0x133EFE41&&a!=0x9D460580&&a!=0x2DB72491&&a!=0x1BB4AF80;
    dp_object *d=new_dp(lobby,wide);if(!d)return E_OUTOFMEMORY_;
    MEM32(out)=d->com.guest;void_log("[lan] created %s (%s)",lobby?"DirectPlayLobby3":"DirectPlay4",wide?"Unicode":"ANSI");return 0;
}
WINAPI_ORDINAL("dplayx.dll",DirectPlayCreate,1,3)
{
    if(!writable(ARG(1),4))return E_POINTER_;MEM32(ARG(1))=0;if(ARG(2))return DP_INVALID;
    dp_object *d=new_dp(0,0);if(!d)return E_OUTOFMEMORY_;MEM32(ARG(1))=d->com.guest;return 0;
}
