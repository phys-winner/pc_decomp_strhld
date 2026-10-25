#include "../android/host/game_text.c"
#include <assert.h>
#include <sys/mman.h>

static int calls,stamp=1234;
void void_log(const char *format,...){(void)format;}
int void_vm_readable(uint32_t address,uint32_t length)
{return address>=0x400000&&(uint64_t)address+length<=0x4000000;}
void void_regs_save(void_regs *r){memset(r,0,sizeof *r);r->eax=(uint32_t)stamp;}
void void_regs_load(const void_regs *r){stamp=(int)r->eax;}
uint32_t void_call_guest_this(uint32_t function,uint32_t self,int count,const uint32_t *args)
{
    assert(self==TEXT_EDITOR);calls++;stamp=0;unsigned bank=MEM32(self);
    if(function==TEXT_CLEAR){
        assert(!count);memset(GPTR(self+336+500*bank),0,500);MEM32(self+144+4*bank)=0;
    }else{
        assert(function==TEXT_CHAR&&count==1);unsigned n=MEM32(self+144+4*bank);
        assert(n<(unsigned)text_limit);MEM16(self+336+500*bank+2*n)=(uint16_t)args[0];MEM32(self+144+4*bank)=n+1;
    }
    return 0;
}
#ifdef STRONGHOLD_ORIGINAL
static void hd_widget_focus(void)
{
    uint16_t text[250],name[]={0x422,0x435,0x441,0x442};int limit,bank;
    MEM32(TEXT_EDITOR)=9;MEM32(TEXT_EDITOR+4)=1;
    stronghold_text_focus(TEXT_EDITOR);
    for(int i=0;i<4;i++)assert(!stronghold_text_frame(26,UINT32_MAX));
    assert(!stronghold_game_editor_get(text,&limit,&bank));
    stronghold_text_focus(TEXT_EDITOR);
    for(int i=0;i<4;i++)assert(!stronghold_text_frame(41,UINT32_MAX));
    assert(!stronghold_game_editor_get(text,&limit,&bank)); /* No deferred intro popup. */
    /* A real profile field at state40 still opens and commits via the original bank. */
    MEM32(TEXT_EDITOR)=0;MEM32(TEXT_EDITOR+80)=20;MEM16(TEXT_EDITOR+336)='A';
    uint32_t node=0x500000;MEM32(node+28)=text_draws[0];stronghold_game_ui_click(node);
    assert(!stronghold_text_frame(40,UINT32_MAX)&&!stronghold_text_frame(40,UINT32_MAX)&&
           stronghold_text_frame(40,UINT32_MAX)==2);
    int token=stronghold_game_editor_get(text,&limit,&bank);assert(token>0&&bank==0&&limit==20);
    int before=calls;assert(stronghold_game_editor_set(name,4,token));
    assert(!stronghold_text_frame(40,UINT32_MAX)&&calls==before+5&&MEM16(TEXT_EDITOR+336)==0x422);
    /* Intro entry also discards a queued replacement from an earlier field. */
    assert(stronghold_game_editor_set(name,1,token));before=calls;
    stronghold_text_focus(TEXT_EDITOR);assert(!stronghold_text_frame(26,UINT32_MAX)&&calls==before);
    assert(!stronghold_game_editor_get(text,&limit,&bank)&&!stronghold_game_editor_set(name,1,token));
    for(int i=0;i<4;i++)assert(!stronghold_text_frame(40,UINT32_MAX));
    /* The save dialog uses a second real text-field draw class. */
    MEM32(node+28)=text_draws[1];
    stronghold_game_ui_click(node);
    assert(!stronghold_text_frame(41,3)&&!stronghold_text_frame(41,3)&&stronghold_text_frame(41,3)==2);
    assert(stronghold_game_editor_get(text,&limit,&bank)>token);
    token=stronghold_game_editor_get(text,&limit,&bank);before=calls;
    assert(stronghold_game_editor_set(name,4,token));
    assert(!stronghold_text_frame(41,3)&&calls==before+5&&MEM16(TEXT_EDITOR+336)==0x422);
    puts("HD editor: hidden initialization focus26/41 ignored; real profile40/save41 field taps, UTF16 commits and stale replacement cancellation PASS");
}
#endif
int main(void)
{
    assert(mmap(GPTR(0x400000),0x3c00000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
    uint16_t text[250],name[]={0x422,0x435,0x441,0x442};int limit,bank;
    stronghold_game_editor_request();assert(!stronghold_text_frame(41,1));
    assert(!stronghold_game_editor_get(text,&limit,&bank)); /* No blank editor without a real focused field. */
    MEM32(TEXT_EDITOR+4)=1;MEM32(TEXT_EDITOR+80)=20;MEM16(TEXT_EDITOR+336)='A';
    stronghold_text_focus(TEXT_EDITOR+4);assert(!stronghold_text_frame(41,1));
    stronghold_text_focus(TEXT_EDITOR);
#ifdef STRONGHOLD_ORIGINAL
    for(int i=0;i<4;i++)assert(!stronghold_text_frame(41,1));
    assert(!stronghold_game_editor_get(text,&limit,&bank));
    MEM32(0x500000+28)=text_draws[0];stronghold_game_ui_click(0x500000);
#endif
    assert(!stronghold_text_frame(41,1)&&!stronghold_text_frame(41,1)&&stronghold_text_frame(41,1)==2);
    int id=stronghold_game_editor_get(text,&limit,&bank);assert(id>0&&limit==20&&bank==0&&text[0]=='A');
    assert(!stronghold_game_editor_set(name,4,id-1)&&!stronghold_game_editor_set(text,21,id));
    assert(stronghold_game_editor_set(name,4,id));assert(!stronghold_text_frame(41,1));
    assert(calls==5&&stamp==1234&&MEM16(TEXT_EDITOR+336)==0x422&&MEM32(TEXT_EDITOR+144)==4);
    for(int i=0;i<8;i++)assert(!stronghold_text_frame(41,1)); /* Cancelling/finishing does not reopen on polling. */
    uint32_t node=0x500000;MEM32(node+28)=0x123456;
    stronghold_game_ui_click(node);for(int i=0;i<3;i++)assert(!stronghold_text_frame(41,1));
    for(unsigned i=0;i<sizeof text_draws/sizeof *text_draws;i++){
        MEM32(node+28)=text_draws[i];stronghold_game_ui_click(node);
        assert(!stronghold_text_frame(41,1)&&!stronghold_text_frame(41,1)&&stronghold_text_frame(41,1)==2);
        int next=stronghold_game_editor_get(text,&limit,&bank);assert(next>id);id=next;
    }
    int before=calls;assert(stronghold_game_editor_set(name,1,id));
    assert(!stronghold_text_frame(41,2)&&calls==before); /* The dialog changed while Android was editing. */
    assert(!stronghold_game_editor_get(text,&limit,&bank)&&!stronghold_game_editor_set(name,1,id));
    MEM32(TEXT_EDITOR)=5;MEM32(TEXT_EDITOR+80+5*4)=16;stronghold_game_editor_request();
    assert(stronghold_text_frame(41,2)==2);
    int next=stronghold_game_editor_get(text,&limit,&bank);assert(next>id&&bank==5&&limit==16);id=next;
    assert(!stronghold_game_editor_set(name,1,id-1));
    assert(stronghold_game_editor_set(name,1,id));MEM32(TEXT_EDITOR)=6;
    assert(!stronghold_text_frame(41,2)&&calls==before); /* A bank change cannot overwrite the new field. */
    MEM32(TEXT_EDITOR)=16;stronghold_game_editor_request();assert(!stronghold_text_frame(41,2));
    MEM32(TEXT_EDITOR)=0;MEM32(TEXT_EDITOR+4)=0;stronghold_game_editor_request();assert(!stronghold_text_frame(41,2));
    MEM32(TEXT_EDITOR+4)=1;MEM32(TEXT_EDITOR+80)=999;stronghold_game_editor_request();
    assert(stronghold_text_frame(41,2)==2);id=stronghold_game_editor_get(text,&limit,&bank);assert(limit==249);
    assert(stronghold_game_editor_set(NULL,0,id));assert(!stronghold_text_frame(41,2)&&calls==before+1&&stamp==1234);
    assert(MEM32(TEXT_EDITOR+144)==0);
#ifdef STRONGHOLD_ORIGINAL
    hd_widget_focus();
#endif
    puts("shared text editor: focused original banks, delayed/repeated widget clicks, UTF16 replacement/limits, stale context/token, original calls/registers PASS");
    return 0;
}
