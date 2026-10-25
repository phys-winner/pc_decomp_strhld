/* Android UI exchanges snapshots/commands with the original guest thread. */
#include "host.h"
#include "game_ui.h"
#include "game_text.h"
#include "edition.h"
#include "display.h"
#include "dplay.h"
#include <string.h>
#ifdef STRONGHOLD_CLASSIC
#define A(c,e) (c##u)
#else
#define A(c,e) (e##u)
#endif
#define GAME A(0x21259A0,0x2BB8E80)
#define OPTIONS A(0x11639F0,0x1163E50)
#define EDITOR A(0x17893D0,0x20BC3F8)
#define SOUND A(0x1165218,0x1165678)
#define RADAR_COLOURS A(0x21288D0,0x2BBBDB0)
#define FLAT_RELIEF A(0x2125764,0x2BB8C44)
#define MAIN_DIALOG A(0x212594C,0x2BB8E2C)
static const uint32_t addresses[SH_GAME_SETTINGS] = {
    A(0x2125A68,0x2BB8F48), A(0x2125A6C,0x2BB8F4C),
    A(0x11684F0,0x1168950), A(0xF69FE8,0xF6A448), SOUND,
    SOUND+0x70, SOUND+0x74, SOUND+0x7c, A(0x21288C0,0x2BBBDA0)
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int events, available, values[SH_GAME_SETTINGS], requested[SH_GAME_SETTINGS];
static unsigned changed,applying;
static int skip_startup, skipped;
static int control_state[SH_CONTROL_STATE], command_keys[16], command_modifiers[16], command_count;
void stronghold_game_control_state(int *out)
{ pthread_mutex_lock(&lock);memcpy(out,control_state,sizeof control_state);pthread_mutex_unlock(&lock); }
void stronghold_game_tools(void)
{ pthread_mutex_lock(&lock);events|=4;pthread_mutex_unlock(&lock); }
void stronghold_game_command(int key,int modifiers)
{
    if(modifiers<0||modifiers>3||!((key>='0'&&key<='9')||key==0x20||key=='Z'||key=='X'||key=='C'||
       key=='R'||key=='H'||key=='V'||key=='E'||key=='D'||key==0xDE||key==0xBC||key==0x6B||key==0x6D))return;
    pthread_mutex_lock(&lock);
    if(command_count<16){command_keys[command_count]=key;command_modifiers[command_count++]=modifiers;}
    pthread_mutex_unlock(&lock);
}

static uint32_t call(uint32_t fn, uint32_t self, int n, const uint32_t *args)
{
    void_regs saved; void_regs_save(&saved);
    uint32_t result = self ? void_call_guest_this(fn,self,n,args) : void_call_guest(fn,n,args,0);
    void_regs_load(&saved); return result;
}
void stronghold_game_ui_skip(int enabled) { skip_startup=enabled; }
int stronghold_game_ui_event(void)
{
    pthread_mutex_lock(&lock); int result=events;events=0;pthread_mutex_unlock(&lock);return result;
}
int stronghold_game_settings_get(int *out)
{
    pthread_mutex_lock(&lock);int ready=available;memcpy(out,values,sizeof values);
    /* Paused frames have not applied Android edits yet. Keep the latest edit
     * visible through the drain-to-publish interval as well as while queued. */
    for(int i=0;i<SH_GAME_SETTINGS;++i)if((changed|applying)&(1u<<i))out[i]=requested[i];
    pthread_mutex_unlock(&lock);return ready;
}
void stronghold_game_setting_set(int index,int value)
{
    if(index<0||index>=SH_GAME_SETTINGS)return;
    int min=index==0?20:index==3?1:0;
    int max=index==0?90:index==2?2:index==3?2:index>=5&&index<=7?100:1;
    if(value<min||value>max)return;
    pthread_mutex_lock(&lock);requested[index]=value;changed|=1u<<index;pthread_mutex_unlock(&lock);
}
static void volume(int type,int amount)
{
    const uint32_t args[]={(uint32_t)type,(uint32_t)amount};
    call(A(0x47A9E0,0x47ABD0),SOUND,2,args);
}
void stronghold_game_ui_frame(void)
{
    if(!void_vm_readable(GAME,28)||!void_vm_readable(EDITOR,8400))return;
    uint32_t state=MEM32(GAME+12),modal=MEM32(OPTIONS+0x5c);
    if(stronghold_dplay_disconnected()){pthread_mutex_lock(&lock);events|=8;pthread_mutex_unlock(&lock);}
    int keys[16],mods[16],count;
    pthread_mutex_lock(&lock);count=command_count;command_count=0;
    memcpy(keys,command_keys,sizeof keys);memcpy(mods,command_modifiers,sizeof mods);pthread_mutex_unlock(&lock);
    for(int i=0;i<count;i++) {
        if(state!=12&&state!=14&&state!=16&&state!=17&&state!=1002)continue;
        if(keys[i]==0xDE&&mods[i]==2&&MEM32(GAME+104)==1&&state!=1002) {
            /* Alt+' reveals the original tax/ration/money fields only in the scenario editor. */
            if(!modal&&MEM32(A(0x212594C,0x2BB8E2C))==UINT32_MAX) {
                uint32_t section=25;call(A(0x4431F0,0x4433C0),0,1,&section);
                if(MEM32(GAME+24)==1002)stronghold_game_command(0xDE,2);
            }
            continue;
        }
        uint32_t ctrl=A(0xF5F934,0xF5FD94),shift=ctrl+4,alt=ctrl+8;
        uint32_t saved[]={MEM32(ctrl),MEM32(shift),MEM32(alt)};
        MEM32(ctrl)=!!(mods[i]&1);MEM32(shift)=0;MEM32(alt)=!!(mods[i]&2);
        uint32_t args[]={MEM32(GAME_SCREEN+0xB0),(mods[i]&2)?0x104:0x100,(uint32_t)keys[i],1};
        void_regs registers;void_regs_save(&registers);
        void_call_guest(A(0x4B3280,0x4B3410),4,args,1);void_regs_load(&registers);
        MEM32(ctrl)=saved[0];MEM32(shift)=saved[1];MEM32(alt)=saved[2];
        /* Android confirms these view toggles: their effect can be invisible,
         * e.g. force colours on a radar without other lords. */
        if(keys[i]=='R'&&mods[i]==2){pthread_mutex_lock(&lock);events|=16;pthread_mutex_unlock(&lock);}
        if(keys[i]==0x20&&!mods[i]){pthread_mutex_lock(&lock);events|=32;pthread_mutex_unlock(&lock);}
    }
    int controls[SH_CONTROL_STATE]={0};
    controls[0]=((state==12||state==14||state==16)?1:0)|(stronghold_dplay_active()?4:0);
    if(state==41&&MEM32(MAIN_DIALOG)==UINT32_MAX)controls[0]|=8; /* Original main menu, no dialog. */
    controls[8]=(MEM8(RADAR_COLOURS)?1:0)|(MEM32(FLAT_RELIEF)?2:0);
    controls[9]=(int)MEM32(addresses[0]);
    if(MEM32(GAME+104)==1)controls[0]|=2; /* Original editor mode. */
    uint32_t units=A(0x14BEBC8,0x1593698),player=A(0x1B6526C,0x25F874C);
    if((controls[0]&1)&&MEM32(units+32)&&MEM32(units)<=20001)for(unsigned id=1;id<MEM32(units);id++) {
        uint32_t u=units+id*1168;
        /* Original group assignment excludes the lord (unit55). */
        if(MEM16(u+1608)&&MEM16(u+1696)==2&&!MEM16(u+2228)&&MEM16(u+1698)!=55&&MEM16(u+1706)==MEM32(player))
            ++controls[1];
    }
    if(controls[0]&1)for(unsigned slot=1;slot<=9;slot++) {
        uint32_t base=A(0x1168500,0x1168960)+slot*20000;
        for(unsigned n=0;n<2500;n++) {
            int32_t id=(int32_t)MEM32(base+n*8);
            if(id==-1)continue; /* Original groups retain holes after casualties. */
            if(id<=0||id>20000||(uint32_t)id>=MEM32(units))continue;
            uint32_t u=units+(uint32_t)id*1168;
            if(MEM32(u+1708)==MEM32(base+n*8+4)&&MEM16(u+1696)==2&&!MEM16(u+2228)&&MEM16(u+1706)==MEM32(player)) {
                controls[2]|=1<<(slot-1);break;
            }
        }
        /* New editor maps leave unused marks zero; tile0 is the unplayable map border. */
        if((int32_t)MEM32(A(0x11B9B6C,0x124C78C)+('0'+slot)*4)>0)controls[3]|=1<<(slot-1);
    }
    controls[4]=(int)stronghold_display_width(1280);controls[5]=(int)stronghold_display_height(720);
    controls[6]=(int32_t)MEM32(A(0xF687E8,0xF68C48));controls[7]=(int32_t)MEM32(A(0xF687EC,0xF68C4C));
    pthread_mutex_lock(&lock);memcpy(control_state,controls,sizeof controls);pthread_mutex_unlock(&lock);
    if(skip_startup&&!skipped&&state==3){
        const uint32_t args[]={48,0};call(A(0x46B430,0x46B610),GAME,2,args);skipped=1;
        void_log("[startup] skipping logos, advertising and online notice");
    }
    /* Close through the original history stack before opening Android settings. */
    if(modal==6||modal==7||modal==13){
        call(A(0x493F70,0x494100),OPTIONS,0,NULL);
        pthread_mutex_lock(&lock);
        events|=SH_UI_SETTINGS|(modal==6?SH_UI_SETTINGS_GRAPHICS:modal==7?SH_UI_SETTINGS_AUDIO:0);
        pthread_mutex_unlock(&lock);
        modal=MEM32(OPTIONS+0x5c);
    }
    int text_event=stronghold_text_frame(state,modal);
    if(text_event){pthread_mutex_lock(&lock);events|=text_event;pthread_mutex_unlock(&lock);}
    if(state==0||state==3||state==48)return; /* Original config/audio initialization comes first. */
    int updates[SH_GAME_SETTINGS];unsigned mask;
    pthread_mutex_lock(&lock);
    mask=changed;changed=0;applying=mask;memcpy(updates,requested,sizeof updates);
    pthread_mutex_unlock(&lock);
    int dirty=0;
    for(int i=0;i<SH_GAME_SETTINGS;i++)if((mask&(1u<<i))&&MEM32(addresses[i])!=(uint32_t)updates[i]){
        int v=updates[i];dirty=1;
        if(i==4)call(v?A(0x47C3F0,0x47C5E0):A(0x47AAB0,0x47ACA0),SOUND,0,NULL);
        else if(i==5)volume(0,v);
        else if(i==6){volume(1,v);volume(2,v);volume(-1,v);}
        else if(i==7){volume(3,v);volume(4,v);}
        else {
            MEM32(addresses[i])=(uint32_t)v;
            if(i==3)call(A(0x468130,0x468330),A(0xF69DF8,0xF6A258),0,NULL);
        }
    }
    if(dirty){call(A(0x496C20,0x496DB0),0,0,NULL);void_log("[settings] applied and saved original game options");}
    pthread_mutex_lock(&lock);
    for(int i=0;i<SH_GAME_SETTINGS;i++)values[i]=(int)MEM32(addresses[i]);available=1;applying=0;
    pthread_mutex_unlock(&lock);
}
