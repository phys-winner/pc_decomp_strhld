/* All three original editors use sixteen 250-character UTF16 banks. Android
 * snapshots/replacements cross threads; original clear/character functions
 * keep each bank's filename/IP/numeric restrictions and cursor bookkeeping. */
#include "host.h"
#include "game_ui.h"
#include "game_text.h"
#include <string.h>
#ifdef STRONGHOLD_ORIGINAL
/* Stronghold.exe 8b581f48: WndProc493AB1, clear461860, char462AE0. */
#define TEXT_EDITOR 0xEC64F0u
#define TEXT_CLEAR 0x461860u
#define TEXT_CHAR 0x462AE0u
static const uint32_t text_draws[]={0x47C960u,0x46AEF0u,0x49D000u};
#elif defined(STRONGHOLD_CLASSIC)
#define TEXT_EDITOR 0x17893D0u
#define TEXT_CLEAR 0x473510u
#define TEXT_CHAR 0x474870u
static const uint32_t text_draws[]={0x4939C0u,0x47D7E0u};
#else
#define TEXT_EDITOR 0x20BC3F8u
#define TEXT_CLEAR 0x473730u
#define TEXT_CHAR 0x474A90u
static const uint32_t text_draws[]={0x493AE0u,0x47D9D0u};
#endif
static pthread_mutex_t text_lock=PTHREAD_MUTEX_INITIALIZER;
static int text_due,text_serial,text_token,text_limit,text_bank;
static uint32_t text_state,text_modal;
static uint16_t text_snapshot[250],text_replacement[250];
static int text_pending,text_length;

static void text_widget_request(void)
{
    pthread_mutex_lock(&text_lock);text_due=3;pthread_mutex_unlock(&text_lock);
}
void stronghold_text_focus(uint32_t editor)
{
#ifdef STRONGHOLD_ORIGINAL
    /* HD also activates hidden banks during intro and main-menu initialization. */
    (void)editor;
#else
    if(editor==TEXT_EDITOR)text_widget_request();
#endif
}
void stronghold_game_ui_click(uint32_t node)
{
    if(!node||!void_vm_readable(node,80))return;
    uint32_t draw=MEM32(node+28);
    for(unsigned i=0;i<sizeof text_draws/sizeof *text_draws;i++)if(draw==text_draws[i]){
        text_widget_request();break;
    }
}
void stronghold_game_editor_request(void)
{ pthread_mutex_lock(&text_lock);text_due=1;pthread_mutex_unlock(&text_lock); }
int stronghold_game_editor_get(uint16_t *text,int *limit,int *bank)
{
    pthread_mutex_lock(&text_lock);
    memcpy(text,text_snapshot,sizeof text_snapshot);*limit=text_limit;*bank=text_bank;
    int token=text_token;pthread_mutex_unlock(&text_lock);return token;
}
int stronghold_game_editor_set(const uint16_t *text,int length,int token)
{
    if(length<0||length>249||(!text&&length))return 0;
    pthread_mutex_lock(&text_lock);
    int ok=token>0&&token==text_token&&length<=text_limit;
    if(ok){if(length)memcpy(text_replacement,text,(size_t)length*2);text_length=length;text_pending=1;}
    pthread_mutex_unlock(&text_lock);return ok;
}
static void text_call(uint32_t function,int count,const uint32_t *args)
{
    void_regs registers;void_regs_save(&registers);
    void_call_guest_this(function,TEXT_EDITOR,count,args);
    void_regs_load(&registers);
}
int stronghold_text_frame(uint32_t state,uint32_t modal)
{
    if(!void_vm_readable(TEXT_EDITOR,8336))return 0;
    unsigned bank=MEM32(TEXT_EDITOR);
    int active=MEM32(TEXT_EDITOR+4)&&bank<16;
    int event=0,replace=0,length=0;uint16_t replacement[250];
    pthread_mutex_lock(&text_lock);
    if(text_token&&(!active||state!=text_state||modal!=text_modal||bank!=(unsigned)text_bank)){
        text_token=0;text_pending=0;
    }
    if(text_due&&!--text_due&&active){
        text_bank=(int)bank;text_limit=(int)MEM32(TEXT_EDITOR+80+bank*4);
        if(text_limit<1||text_limit>249)text_limit=249;
        memcpy(text_snapshot,GPTR(TEXT_EDITOR+336+500*bank),sizeof text_snapshot);text_snapshot[249]=0;
        text_state=state;text_modal=modal;text_serial=text_serial==0x7fffffff?1:text_serial+1;
        text_token=text_serial;event=2;
    }
    if(text_pending){
        replace=text_token>0&&active&&state==text_state&&modal==text_modal&&bank==(unsigned)text_bank;
        length=text_length;memcpy(replacement,text_replacement,sizeof replacement);text_pending=0;
    }
    pthread_mutex_unlock(&text_lock);
    if(replace){
        text_call(TEXT_CLEAR,0,NULL);
        for(int i=0;i<length;i++){uint32_t ch=replacement[i];text_call(TEXT_CHAR,1,&ch);}
        void_log("[editor] replaced bank %u, length %u",bank,MEM32(TEXT_EDITOR+144+bank*4));
    }
    return event;
}
