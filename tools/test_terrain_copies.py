"""Compare original lifted terrain diamonds with native row copies in all three editions."""
import argparse
import json
import re
import subprocess
from pathlib import Path

import build_android
import display_hooks
from test_display_rle import HEADER

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/terrain-copies'
LAYOUT={
    'classic':(0xF1059C,0xF105AC,0xF105A8,0xF105BC,0xF105C0,0xD0DA58,0x2128950,0x212892C,0x2128930,0x2128934,0xFD5858,0xFD595C),
    'game':(0xF109FC,0xF10A0C,0xF10A08,0xF10A1C,0xF10A20,0xD0DBD8,0x2BBBE30,0x2BBBE0C,0x2BBBE10,0x2BBBE14,0xFD5CB8,0xFD5DBC),
    'stronghold':(0xA656A0,0xA656B0,0xA656AC,0xA656C0,0xA656C4,0x966440,0x1505708,0x15056E4,0x15056E8,0x15056EC,0xB183A0,0xB184A4),
}
FOOTER=r'''
#include <time.h>
static uint8_t reference[1048576],stack_reference[128];
static void snapshot(uint32_t *out){
 uint32_t v[]={g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_flag_k,g_flag_a,g_flag_b,g_flag_cf,MEM32(LAST)};
 memcpy(out,v,sizeof(v));
}
static void setup(int native,int y,int phase,int overlap){
 memset(PTR(0x12000000),0xA5,sizeof(reference));memset(PTR(0x15007F80),0x5A,128);
 uint32_t source=(overlap?0x12000020:0x14000000)+phase;
 uint8_t *pixels=PTR(source);for(unsigned i=0;i<512;i++)pixels[i]=(uint8_t)(i*97+i/7);
 MEM32(Y)=y+5;MEM32(Y_ORIGIN)=8;MEM32(Y_OFFSET)=3;
 MEM32(TILE)=3;MEM32(X)=phase+4;MEM32(TABLE+12)=source-16;MEM32(SOURCE_ADD)=16;
 MEM32(NATIVE)=native;MEM32(CLIP_TOP)=0;MEM32(CLIP_BOTTOM)=32;
 MEM32(DEST)=0x12000000+phase;MEM32(LAST)=0x11223344;
 g_eax=0x11223344;g_ecx=0x22334455;g_edx=0x33445566;g_ebx=0x44556677;
 g_esi=0x55667788;g_edi=0x66778899;g_ebp=0x778899AA;g_esp=0x15008000;
 g_flag_k=g_flag_a=g_flag_b=g_flag_cf=g_backedges=0;recomp_yield_hook=NULL;
}
static double now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.+t.tv_nsec/1e6;}
int main(void){
 assert(mmap((void*)VOID_GUEST_BASE,VOID_GUEST_SPAN,PROT_READ|PROT_WRITE,
             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_NORESERVE,-1,0)==(void*)VOID_GUEST_BASE);
 unsigned cases=0;uint32_t expected[13],actual[13];
 for(int native=0;native<2;native++)for(int y=-1;y<=32;y++)for(int phase=0;phase<4;phase++)for(int overlap=0;overlap<2;overlap++){
  setup(native,y,phase,overlap);terrain_original();snapshot(expected);
  memcpy(reference,PTR(0x12000000),sizeof(reference));memcpy(stack_reference,PTR(0x15007F80),128);
  setup(native,y,phase,overlap);terrain_rows();snapshot(actual);
  assert(!memcmp(reference,PTR(0x12000000),sizeof(reference)));
  assert(!memcmp(stack_reference,PTR(0x15007F80),128));assert(!memcmp(expected,actual,sizeof(expected)));
  assert(g_backedges==0);cases++;
 }
 printf("terrain: %u exact pixel/register/flags/stack cases, full/half parity, clipping, unaligned and overlapping buffers PASS\n",cases);
 for(int native=0;native<2;native++)for(int parity=0;parity<(native?2:1);parity++){
  setup(native,12+parity,0,0);double before=now_ms();
  for(unsigned n=0;n<200000;n++){g_esp=0x15008000;terrain_original();}
  double original=now_ms()-before;setup(native,12+parity,0,0);before=now_ms();
  for(unsigned n=0;n<200000;n++){g_esp=0x15008000;terrain_rows();}
  printf("terrain native%d parity%d: %.3f -> %.3f ms\n",native,parity,original,now_ms()-before);
 }
 return 0;
}
'''


def build(module):
    base=display_hooks.TERRAIN_COPIES[module]
    pattern=rb'(?ms)^void sub_%08X\(void\) \{.*?^\}'%base
    body=next(match[0] for path in (ROOT/f'generated/{module}-x86').glob('recomp_*.c')
              if (match:=re.search(pattern,path.read_bytes())))
    rows,count=display_hooks.collapse_terrain_copies(body,module)
    assert count==3 and rows.count(b'__restrict _terrain_s')==3
    # Refuse a changed original store, instead of silently using an obsolete layout.
    changed=body.replace(b'MEM32(edi + 0x1C) = eax;',b'MEM32(edi + 0x1C) = edi;',1)
    try:display_hooks.collapse_terrain_copies(changed,module)
    except ValueError:pass
    else:raise AssertionError('changed terrain copy accepted')
    names=('Y','Y_ORIGIN','Y_OFFSET','TILE','X','TABLE','SOURCE_ADD','NATIVE','CLIP_TOP','CLIP_BOTTOM','DEST','LAST')
    definitions=''.join(f'#define {name} 0x{value:X}u\n' for name,value in zip(names,LAYOUT[module]))
    source=HEADER+'\n'+definitions+body.replace(b'sub_%08X'%base,b'terrain_original',1).decode()+'\n'+rows.replace(
        b'sub_%08X'%base,b'terrain_rows',1).decode()+'\n'+FOOTER
    folder=OUT/module;folder.mkdir(parents=True,exist_ok=True)
    path=folder/'terrain-check.c';path.write_text(source)
    binary=folder/'terrain-check'
    command=[str(build_android.CLANG),'--target='+build_android.TARGET,'-O2','-std=gnu11',
             '-I'+str(ROOT/'android/runtime'),str(path),'-o',str(binary)]
    subprocess.run(command,check=True)
    return binary


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    args=parser.parse_args()
    results=[]
    for module in LAYOUT:
        binary=build(module)
        if args.serial:
            remote=f'/data/local/tmp/terrain-copy-{module}'
            adb=[str(build_android.SDK/'platform-tools/adb.exe'),'-s',args.serial]
            subprocess.run([*adb,'push',str(binary),remote],check=True)
            result=subprocess.run([*adb,'shell',f'chmod 700 {remote} && {remote}'],capture_output=True,text=True)
            print(result.stdout,end='');result.check_returncode()
            results.append(dict(module=module,status='PASS',output=result.stdout))
        else:print(module, 'built', binary)
    if results:(OUT/'acceptance.json').write_text(json.dumps(results,indent=2)+'\n')
