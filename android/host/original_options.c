/* Stronghold.exe SHA8b581f48: config writer47F2C0 and original options
 * handlers47D290/47D460/47BF60. Android queues changes; the guest applies them. */
#ifdef STRONGHOLD_ORIGINAL
#include "host.h"
#include "game_ui.h"
#include "original_options.h"
#include <string.h>
#define OPTIONS_SOUND 0xBC8AE0u
static const uint32_t option_addresses[]={0x1503DE4u,0x1503DE8u,0xBCAFA8u,0xABC54Cu,
    OPTIONS_SOUND,OPTIONS_SOUND+0x70,OPTIONS_SOUND+0x74,OPTIONS_SOUND+0x7C};
static pthread_mutex_t options_lock=PTHREAD_MUTEX_INITIALIZER;
static int options_ready,options_values[SH_GAME_SETTINGS],options_requested[8];
static unsigned options_changed,options_applying;
int stronghold_game_settings_get(int *values)
{
    pthread_mutex_lock(&options_lock);memcpy(values,options_values,sizeof options_values);
    /* Android settings may reopen while the guest is paused, or during an
     * original callback after the queue drains and before snapshot publish. */
    for(int i=0;i<8;++i)if((options_changed|options_applying)&(1u<<i))values[i]=options_requested[i];
    int ready=options_ready;pthread_mutex_unlock(&options_lock);return ready;
}
void stronghold_game_setting_set(int index,int value)
{
    if(index<0||index>=8)return; /* The original Stronghold has no Crusader genie. */
    int min=index==0?10:index==3?1:0;
    int max=index==0?90:index==2?2:index==3?2:index>=5?100:1;
    if(value<min||value>max)return;
    pthread_mutex_lock(&options_lock);options_requested[index]=value;options_changed|=1u<<index;
    pthread_mutex_unlock(&options_lock);
}
static void options_call(uint32_t function,uint32_t self,int count,const uint32_t *args)
{
    void_regs saved;void_regs_save(&saved);
    if(self)void_call_guest_this(function,self,count,args);else void_call_guest(function,count,args,0);
    void_regs_load(&saved);
}
static void options_volume(int type,int value)
{
    const uint32_t args[]={(uint32_t)type,(uint32_t)value};options_call(0x4680F0u,OPTIONS_SOUND,2,args);
}
int stronghold_original_options_frame(void)
{
    if(!void_vm_readable(0x1503D38u,4)||!void_vm_readable(OPTIONS_SOUND,0x80)||
        !void_vm_readable(0xBC7314u,4))return 0;
    uint32_t state=MEM32(0x1503D38u);
    if(state==0||state==3||state==48)return 0; /* Original config/audio initialization first. */
    int event=0;uint32_t modal=MEM32(0xBC7314u);
    if((modal==6||modal==7||modal==13)&&MEM32(0x1503CE4u)==modal){
        /* Close the original options history before opening Android settings. */
        options_call(0x47CE90u,0xBC72E8u,0,NULL);
        event=SH_UI_SETTINGS|(modal==6?SH_UI_SETTINGS_GRAPHICS:modal==7?SH_UI_SETTINGS_AUDIO:0);
    }
    int changes[8];unsigned mask;
    pthread_mutex_lock(&options_lock);mask=options_changed;options_changed=0;options_applying=mask;
    memcpy(changes,options_requested,sizeof changes);pthread_mutex_unlock(&options_lock);
    int dirty=0;
    for(int i=0;i<8;++i)if((mask&(1u<<i))&&MEM32(option_addresses[i])!=(uint32_t)changes[i]){
        int value=changes[i];dirty=1;
        if(i==4)options_call(value?0x469A50u:0x4681C0u,OPTIONS_SOUND,0,NULL);
        else if(i==5)options_volume(0,value);
        else if(i==6){options_volume(1,value);options_volume(2,value);options_volume(-1,value);}
        else if(i==7){options_volume(3,value);options_volume(4,value);}
        else{
            MEM32(option_addresses[i])=(uint32_t)value;
            if(i==3)options_call(0x4590F0u,0xABC368u,0,NULL);
        }
    }
    if(dirty){options_call(0x47F2C0u,0,0,NULL);void_log("[settings] applied and saved original game options");}
    pthread_mutex_lock(&options_lock);
    for(int i=0;i<8;++i)options_values[i]=(int)MEM32(option_addresses[i]);
    options_values[8]=0;options_ready=1;options_applying=0;
    pthread_mutex_unlock(&options_lock);return event;
}
#endif
