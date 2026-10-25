#include "../android/host/game_text.c"
#include "../android/host/game_ui.c"
#include <assert.h>
#include <sys/mman.h>
static int saves, calls, stamp=1234;
static int key_calls,last_key,last_modifiers;
static int editor_transitions;
static int pending_probe;
int stronghold_dplay_active(void){return 0;}
int stronghold_dplay_disconnected(void){return 0;}
uint32_t stronghold_display_width(uint32_t n){return n;}
uint32_t stronghold_display_height(uint32_t n){return n;}
void void_log(const char *format,...) { (void)format; }
int void_vm_readable(uint32_t va,uint32_t length)
{ return va>=0x400000&&(uint64_t)va+length<=0x4000000; }
void void_regs_save(void_regs *r) { memset(r,0,sizeof *r);r->eax=(uint32_t)stamp; }
void void_regs_load(const void_regs *r) { stamp=(int)r->eax; }
uint32_t void_call_guest_this(uint32_t fn,uint32_t self,int count,const uint32_t *args)
{
    (void)count;calls++;stamp=0;
    if(fn==A(0x46B430,0x46B610))MEM32(GAME+24)=args[0];
    else if(fn==A(0x493F70,0x494100))MEM32(OPTIONS+0x5c)=MEM32(OPTIONS+0x60);
    else if(fn==A(0x473510,0x473730)){
        unsigned bank=MEM32(EDITOR);memset(GPTR(EDITOR+336+bank*500),0,500);MEM32(EDITOR+144+bank*4)=0;
    } else if(fn==A(0x474870,0x474A90)){
        unsigned bank=MEM32(EDITOR),length=MEM32(EDITOR+144+bank*4);
        MEM16(EDITOR+336+bank*500+length*2)=(uint16_t)args[0];MEM32(EDITOR+144+bank*4)=length+1;
    } else if(fn==A(0x47C3F0,0x47C5E0))MEM32(SOUND)=1;
    else if(fn==A(0x47AAB0,0x47ACA0))MEM32(SOUND)=0;
    else if(fn==A(0x47A9E0,0x47ABD0)){
        if(pending_probe){
            int snapshot[SH_GAME_SETTINGS];assert(stronghold_game_settings_get(snapshot)&&snapshot[6]==65);
            assert(applying&(1u<<6)); /* Queue drained; the old snapshot is still55. */
            stronghold_game_setting_set(6,70);
            assert(stronghold_game_settings_get(snapshot)&&snapshot[6]==70);pending_probe=0;
        }
        if(args[0]!=UINT32_MAX)MEM32(SOUND+0x70+args[0]*4)=args[1];
    } else assert(fn==A(0x468130,0x468330)&&self==A(0xF69DF8,0xF6A258));
    return 0;
}
uint32_t void_call_guest(uint32_t fn,int count,const uint32_t *args,int pops)
{
    stamp=0;
    if(fn==A(0x4431F0,0x4433C0)) {
        assert(count==1&&pops==0&&args[0]==25);editor_transitions++;MEM32(GAME+24)=1002;return 0;
    }
    if(fn==A(0x4B3280,0x4B3410)) {
        assert(count==4&&pops==1);key_calls++;last_key=(int)args[2];
        uint32_t ctrl=A(0xF5F934,0xF5FD94);
        last_modifiers=(MEM32(ctrl)?1:0)|(MEM32(ctrl+8)?2:0);
        assert(args[1]==((last_modifiers&2)?0x104u:0x100u)&&!MEM32(ctrl+4));return 0;
    }
    assert(fn==A(0x496C20,0x496DB0));saves++;return 1;
}
int main(void)
{
    assert(mmap((void *)(VOID_GUEST_BASE+0x400000),0x3c00000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
    stronghold_game_setting_set(0,60);stronghold_game_ui_frame();assert(!available&&changed==1);
    MEM32(GAME+12)=3;stronghold_game_ui_skip(1);stronghold_game_ui_frame();assert(MEM32(GAME+24)==48&&stamp==1234);
    int before=calls;stronghold_game_ui_frame();assert(calls==before);
    MEM32(GAME+12)=41;stronghold_game_ui_frame();assert(MEM32(addresses[0])==60&&saves==1);
    int snapshot[9];assert(stronghold_game_settings_get(snapshot)&&snapshot[0]==60);
    stronghold_game_setting_set(0,900);assert(!changed);
    for(int i=1;i<9;i++)stronghold_game_setting_set(i,i==2?2:i==3?2:i>=5&&i<=7?55:1);
    stronghold_game_ui_frame();assert(saves==2&&MEM32(SOUND)==1&&MEM32(addresses[5])==55&&MEM32(addresses[6])==55&&MEM32(addresses[7])==55);
    stronghold_game_setting_set(4,0);stronghold_game_ui_frame();assert(!MEM32(SOUND)&&saves==3&&stamp==1234);
    for(int modal=6;modal<=13;modal++)if(modal==6||modal==7||modal==13){
        MEM32(OPTIONS+0x5c)=modal;MEM32(OPTIONS+0x60)=1;
        stronghold_game_ui_frame();
        int expected=SH_UI_SETTINGS|(modal==6?SH_UI_SETTINGS_GRAPHICS:modal==7?SH_UI_SETTINGS_AUDIO:0);
        assert(MEM32(OPTIONS+0x5c)==1&&stronghold_game_ui_event()==expected&&stronghold_game_ui_event()==0);
    }
    uint32_t node=0x500000;MEM32(node+28)=A(0x4939C0,0x493AE0);
    MEM32(EDITOR+4)=1;MEM32(EDITOR+80)=20;MEM16(EDITOR+336)='A';
    stronghold_game_ui_click(node);stronghold_game_ui_frame();stronghold_game_ui_frame();stronghold_game_ui_frame();
    assert(stronghold_game_ui_event()==2&&stronghold_game_ui_event()==0);
    uint16_t text[250],name[]={0x422,0x435,0x441,0x442};int limit,bank;
    int id=stronghold_game_editor_get(text,&limit,&bank);assert(id>0&&limit==20&&bank==0&&text[0]=='A');
    assert(!stronghold_game_editor_set(name,4,id-1)&&!stronghold_game_editor_set(text,21,id));
    assert(stronghold_game_editor_set(name,4,id));stronghold_game_ui_frame();
    assert(MEM16(EDITOR+336)==0x422&&MEM32(EDITOR+144)==4&&stamp==1234);
    assert(stronghold_game_editor_set(name,1,id));MEM32(OPTIONS+0x5c)=2;
    stronghold_game_ui_frame();assert(MEM32(EDITOR+144)==4); /* Dialog changed while the IME was open. */
    stronghold_game_editor_request();stronghold_game_ui_frame();assert(stronghold_game_ui_event()==2);
    stronghold_game_command('1',3);stronghold_game_ui_frame();assert(key_calls==0); /* Menus discard world commands. */
    MEM32(GAME+12)=14;MEM32(GAME+104)=1;
    uint32_t ctrl=A(0xF5F934,0xF5FD94);MEM32(ctrl)=7;MEM32(ctrl+4)=8;MEM32(ctrl+8)=9;
    stronghold_game_command('1',3);stronghold_game_command('Q',0);stronghold_game_command('2',4);
    stronghold_game_ui_frame();assert(key_calls==1&&last_key=='1'&&last_modifiers==3&&stamp==1234);
    assert(MEM32(ctrl)==7&&MEM32(ctrl+4)==8&&MEM32(ctrl+8)==9);
    for(int i=0;i<20;i++)stronghold_game_command('Z',0);
    stronghold_game_ui_frame();assert(key_calls==17&&last_key=='Z'&&!last_modifiers);
    uint32_t units=A(0x14BEBC8,0x1593698),groups=A(0x1168500,0x1168960),marks=A(0x11B9B6C,0x124C78C);
    memset(GPTR(groups),255,200000);for(unsigned i=0;i<10;i++)MEM32(marks+4*('0'+i))=UINT32_MAX;
    MEM32(A(0x1B6526C,0x25F874C))=1;MEM32(units)=3;MEM32(units+32)=1;
    uint32_t u=units+1168*2;MEM16(u+1696)=2;MEM16(u+1698)=22;MEM16(u+1608)=1;MEM16(u+1706)=1;MEM32(u+1708)=123;
    MEM32(groups+20000+8)=2;MEM32(groups+20000+12)=123;MEM32(marks+4*'3')=2345;
    stronghold_game_ui_frame();int controls[SH_CONTROL_STATE];stronghold_game_control_state(controls);
    assert(controls[0]==3&&controls[1]==1&&controls[2]==1&&controls[3]==4); /* Live unit after a casualty hole. */
    MEM16(u+1698)=55;stronghold_game_ui_frame();stronghold_game_control_state(controls);assert(controls[1]==0);
    MEM32(marks+4*'3')=0;stronghold_game_ui_frame();stronghold_game_control_state(controls);assert(controls[3]==0);
    MEM16(u+2228)=1;stronghold_game_ui_frame();stronghold_game_control_state(controls);assert(controls[2]==0);
    stronghold_game_tools();assert(stronghold_game_ui_event()==4&&stronghold_game_ui_event()==0);
    MEM32(GAME+12)=12;MEM32(OPTIONS+0x5c)=0;MEM32(A(0x212594C,0x2BB8E2C))=UINT32_MAX;
    before=key_calls;stronghold_game_command(0xDE,2);stronghold_game_ui_frame();
    assert(editor_transitions==1&&key_calls==before&&command_count==1);
    MEM32(GAME+12)=1002;stronghold_game_ui_frame();
    assert(key_calls==before+1&&last_key==0xDE&&last_modifiers==2&&!command_count);
    /* A paused guest must not make a recreated Settings screen see stale55.
     * A second edit during an original callback must survive this publish. */
    stronghold_game_setting_set(6,65);
    assert(MEM32(addresses[6])==55&&stronghold_game_settings_get(snapshot)&&snapshot[6]==65);
    pending_probe=1;stronghold_game_ui_frame();
    assert(!pending_probe&&MEM32(addresses[6])==65&&stronghold_game_settings_get(snapshot)&&snapshot[6]==70);
    stronghold_game_ui_frame();assert(MEM32(addresses[6])==70&&stronghold_game_settings_get(snapshot)&&snapshot[6]==70);
    assert(!changed&&!applying&&stamp==1234);
    puts("game UI: startup, original callbacks/save, editor limits/stale context, paused settings snapshot and in-flight newer edit retained PASS");
    return 0;
}
