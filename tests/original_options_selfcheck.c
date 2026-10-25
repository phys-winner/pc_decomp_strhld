#define STRONGHOLD_ORIGINAL
#include "../android/host/original_options.c"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
static int stamp=1234,calls,saves,volumes,closes,cursors,mutes,resumes;
static int pending_probe;
void void_log(const char *format,...){(void)format;}
int void_vm_readable(uint32_t a,uint32_t n){return a>=0x400000&&(uint64_t)a+n<=0x4000000;}
void void_regs_save(void_regs *r){memset(r,0,sizeof *r);r->eax=stamp;}
void void_regs_load(const void_regs *r){stamp=r->eax;}
uint32_t void_call_guest_this(uint32_t fn,uint32_t self,int count,const uint32_t *args)
{
    calls++;stamp=0;
    if(fn==0x47CE90u){
        assert(self==0xBC72E8u&&!count);closes++;
        MEM32(0xBC7314u)=MEM32(0xBC7318u);MEM32(0x1503CE4u)=MEM32(0xBC7314u);
    }else if(fn==0x4590F0u){assert(self==0xABC368u&&!count);cursors++;}
    else if(fn==0x4681C0u){assert(self==OPTIONS_SOUND&&!count);mutes++;MEM32(OPTIONS_SOUND)=0;}
    else if(fn==0x469A50u){assert(self==OPTIONS_SOUND&&!count);resumes++;MEM32(OPTIONS_SOUND)=1;}
    else{
        assert(fn==0x4680F0u&&self==OPTIONS_SOUND&&count==2);volumes++;
        if(pending_probe){
            int snapshot[SH_GAME_SETTINGS];assert(stronghold_game_settings_get(snapshot)&&snapshot[6]==65);
            assert(options_applying&(1u<<6));
            stronghold_game_setting_set(6,70);
            assert(stronghold_game_settings_get(snapshot)&&snapshot[6]==70);pending_probe=0;
        }
        int type=(int)args[0];assert(type>=-1&&type<=4&&args[1]<=100);
        MEM32(OPTIONS_SOUND+(type<0?2264*4:0x70+type*4))=args[1];
    }
    return 0;
}
uint32_t void_call_guest(uint32_t fn,int count,const uint32_t *args,int pops)
{
    (void)args;assert(fn==0x47F2C0u&&!count&&!pops);calls++;saves++;stamp=0;return 0;
}
int main(void)
{
    assert(mmap(GPTR(0x400000),0x3c00000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
    int values[SH_GAME_SETTINGS];assert(!stronghold_game_settings_get(values));
    int initial[]={40,1,0,2,1,80,80,85};
    for(int i=0;i<8;++i)MEM32(option_addresses[i])=initial[i];MEM32(OPTIONS_SOUND+8)=1;
    stronghold_game_setting_set(0,10);stronghold_game_setting_set(8,1);stronghold_game_setting_set(-1,5);
    MEM32(0x1503D38u)=3;assert(!stronghold_original_options_frame()&&!calls);
    MEM32(0x1503D38u)=48;assert(!stronghold_original_options_frame()&&!calls);
    assert(!stronghold_game_settings_get(values));
    int desired[]={10,0,2,1,0,50,60,70};
    for(int i=1;i<8;++i)stronghold_game_setting_set(i,desired[i]);
    stronghold_game_setting_set(0,9);stronghold_game_setting_set(0,91);
    stronghold_game_setting_set(1,2);stronghold_game_setting_set(2,3);stronghold_game_setting_set(3,0);
    stronghold_game_setting_set(5,101);stronghold_game_setting_set(7,-1);
    MEM32(0x1503D38u)=41;assert(!stronghold_original_options_frame());
    assert(stronghold_game_settings_get(values)&&!memcmp(values,desired,sizeof desired)&&!values[8]);
    assert(saves==1&&volumes==6&&cursors==1&&mutes==1&&!resumes&&calls==9&&stamp==1234);
    assert(MEM32(OPTIONS_SOUND+2264*4)==60&&MEM32(OPTIONS_SOUND+0x78)==60&&MEM32(OPTIONS_SOUND+0x80)==70);
    stronghold_original_options_frame();assert(calls==9); /* Equal settings do not save again. */
    stronghold_game_setting_set(4,1);stronghold_original_options_frame();assert(resumes==1&&saves==2&&stamp==1234);
    for(int modal=6;modal<=13;modal+=modal==6?1:6){
        MEM32(0xBC7314u)=modal;MEM32(0xBC7318u)=11;MEM32(0x1503CE4u)=modal;
        int expected=SH_UI_SETTINGS|(modal==6?SH_UI_SETTINGS_GRAPHICS:modal==7?SH_UI_SETTINGS_AUDIO:0);
        assert(stronghold_original_options_frame()==expected&&MEM32(0xBC7314u)==11&&stamp==1234);
    }
    assert(closes==3);MEM32(0xBC7314u)=7;MEM32(0x1503CE4u)=99;
    assert(!stronghold_original_options_frame()&&closes==3); /* An overlapping different modal wins. */
    stronghold_game_setting_set(6,65);
    assert(MEM32(option_addresses[6])==60&&stronghold_game_settings_get(values)&&values[6]==65);
    pending_probe=1;stronghold_original_options_frame();
    assert(!pending_probe&&MEM32(option_addresses[6])==65&&stronghold_game_settings_get(values)&&values[6]==70);
    stronghold_original_options_frame();assert(MEM32(option_addresses[6])==70&&stronghold_game_settings_get(values)&&values[6]==70);
    assert(!options_changed&&!options_applying&&stamp==1234);
    puts("HD options: startup, eight supported settings/no genie, original callbacks/save, modal history, paused snapshot and in-flight newer edit retained PASS");
    return 0;
}
