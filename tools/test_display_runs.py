"""Compare actual lifted TGX stores and coverage before/after span batching."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
import build_android
import display_hooks
from test_display_rle import HEADER

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/display-runs'
FOOTER=r'''
static uint8_t coverage[8112*8],reference[8112*8],expected_coverage[8112*8];
static unsigned calls;
int stronghold_display_capture_active=1;
void stronghold_display_written(uint32_t address,unsigned bytes){
 assert(address>=0x12000000&&address+bytes<=0x12000000+sizeof(coverage));
 memset(coverage+address-0x12000000,255,bytes);calls++;
}
static void setup(unsigned n,unsigned kind,int clip,int x,int y,unsigned seed){
 memset(PTR(0x12000000),0xA5,sizeof(coverage));memset(coverage,0,sizeof(coverage));
 uint8_t *p=PTR(0x14000000);
 for(unsigned row=0;row<6;row++){
  *p++=0x20;*p++=(kind?0x40:0)|(n-1);
  for(unsigned col=0;col<(kind?1:n);col++){
   uint16_t v=col%3==0?0:col%3==1?0xF81F:0x07FF;memcpy(p,&v,2);p+=2;
  }
  *p++=0x80;
 }
 MEM32(RENDERER+8)=1;MEM32(RENDERER+0x16C854)=clip?3:0;
 MEM32(RENDERER+0x16C858)=clip?1:0;MEM32(RENDERER+0x16C85C)=clip?n+3:64;
 MEM32(RENDERER+0x16C860)=clip?4:6;MEM32(SCREEN+0xD8)=0x12000000;
 g_eax=0x11223344;g_ecx=RENDERER;g_edx=0x55667788;g_ebx=0x1234;
 g_esi=0x5678;g_edi=0x9ABC;g_ebp=0xDEAD;g_esp=0x15008000;
 uint32_t args[]={0xDEADC0DE,x,y,n+1,6,0x14000000};memcpy(PTR(g_esp),args,sizeof(args));
 g_flag_k=g_flag_a=g_flag_b=g_flag_cf=0;g_backedges=seed;
 recomp_yield_hook=yield_record;pixel=yield_count=calls=0;
}
static void snapshot(uint32_t *out){
 uint32_t a[]={g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,
               g_flag_k,g_flag_a,g_flag_b,g_flag_cf};memcpy(out,a,sizeof(a));
}
int main(void){
 assert(mmap((void*)VOID_GUEST_BASE,VOID_GUEST_SPAN,PROT_READ|PROT_WRITE,
             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_NORESERVE,-1,0)==(void*)VOID_GUEST_BASE);
 unsigned cases=0,old_calls=0,new_calls=0;uint32_t expected[12],actual[12];
 for(unsigned kind=0;kind<2;kind++)for(unsigned n=1;n<=32;n++)
 for(int clip=0;clip<2;clip++)for(int x=0;x<4;x++)for(int y=0;y<2;y++){
  setup(n,kind,clip,x,y,65520);run_original();snapshot(expected);
  memcpy(reference,PTR(0x12000000),sizeof(reference));memcpy(expected_coverage,coverage,sizeof(coverage));
  unsigned budget=g_backedges,yields=yield_count;old_calls+=calls;
  setup(n,kind,clip,x,y,65520);run_batched();snapshot(actual);new_calls+=calls;
  assert(!memcmp(reference,PTR(0x12000000),sizeof(reference)));
  assert(!memcmp(expected_coverage,coverage,sizeof(coverage)));
  assert(!memcmp(expected,actual,sizeof(expected))&&g_backedges==budget&&yield_count==yields);
  cases++;
 }
 assert(new_calls<old_calls/4);
 printf("TGX opaque runs: %u cases, exact pixels/coverage/registers/flags/backedges/yields PASS; callbacks %u -> %u\n",cases,old_calls,new_calls);
 return 0;
}
'''

def terrain_audit(module):
    lo,hi=display_hooks.TERRAIN_RASTER[module]
    files=[p.read_bytes() for p in (ROOT/f'generated/{module}-x86').glob('recomp_*.c')]
    entries={int(m[1],16) for data in files for m in re.finditer(rb'^void sub_([0-9A-F]{8})\(void\)',data,re.M)
             if lo<=int(m[1],16)<hi}
    pattern=re.compile(rb'RECOMP_(?:CALL|TAIL)\(sub_('+b'|'.join(f'{a:08X}'.encode() for a in sorted(entries))+
                       rb')\); /\* 0x([0-9A-F]{8}):')
    callers={(int(m[2],16),int(m[1],16)) for data in files for m in pattern.finditer(data)}
    assert len(callers)==7 and all(0x4E9690<=pc<0x4ED400 for pc,_ in callers),callers
    stores={int(m[2],16):m[1] for data in files for m in re.finditer(
        rb'(?m)^(    MEM(?:16|32)\([^\r\n]+/\* 0x([0-9A-F]{8}):[^\r\n]+)',data)
        if lo<=int(m[2],16)<hi or int(m[2],16)==display_hooks.OPAQUE_RUNS[module][0][0]}
    # The real terrain stores are untouched; the adjacent UI TGX writer still captures.
    source=b'#include "recomp_funcs.h"\n'+b'\n'.join(stores[pc] for pc in sorted(stores) if lo<=pc<hi)+b'\n'
    assert display_hooks.apply(source,module)[0]==source
    ui=b'#include "recomp_funcs.h"\n'+stores[display_hooks.OPAQUE_RUNS[module][0][0]]+b'\n'
    assert b'stronghold_display_written(edi,2)' in display_hooks.apply(ui,module)[0]
    return {'entries':len(entries),'original_stores':len(stores)-1,
            'world_callers':[[hex(pc),hex(fn)] for pc,fn in sorted(callers)]}

def build(module):
    address=0x454C60 if module=='classic' else 0x454EA0
    pattern=rb'(?ms)^void sub_%08X\(void\) \{.*?^\}'%address
    original=next(m[0] for p in (ROOT/f'generated/{module}-x86').glob('recomp_*.c')
                  if (m:=re.search(pattern,p.read_bytes())))
    for store,_ in display_hooks.OPAQUE_RUNS[module]:
        pattern=rb'(MEM16\(edi\) = [^;]+;)( /\* 0x%08X:)'%store
        original,n=re.subn(pattern,rb'\1 if(stronghold_display_capture_active) stronghold_display_written(edi,2);\2',original)
        assert n==1
    batched,count=display_hooks.batch_opaque_runs(original,module);assert count==4
    original=original.replace(b'sub_%08X'%address,b'run_original',1)
    batched=batched.replace(b'sub_%08X'%address,b'run_batched',1)
    screen,renderer=(0xFD5780,0x21288D8) if module=='classic' else (0xFD5BE0,0x2BBBDB8)
    declarations=f'\n#define SCREEN 0x{screen:X}u\n#define RENDERER 0x{renderer:X}u\nextern int stronghold_display_capture_active;\nvoid stronghold_display_written(uint32_t,unsigned);\n'
    source=OUT/f'{module}.c';binary=OUT/module
    source.write_bytes(HEADER.encode()+declarations.encode()+original+b'\n'+batched+FOOTER.encode())
    subprocess.run([str(build_android.CLANG),f'--target=aarch64-linux-android{build_android.API}',
                    '-O2','-std=gnu11','-fno-strict-aliasing','-fwrapv','-Iandroid/runtime',
                    str(source),'-Wl,-z,max-page-size=16384','-lm','-o',str(binary)],check=True,cwd=ROOT)
    return {'edition':module,'binary':str(binary),'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
            'terrain':terrain_audit(module)}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--device');args=parser.parse_args()
    OUT.mkdir(parents=True,exist_ok=True);results=[]
    for module in ('classic','game'):
        result=build(module)
        if args.device:
            prefix=['adb','-s',args.device];remote='/data/local/tmp/stronghold-runs-'+module
            subprocess.run([*prefix,'push',result['binary'],remote],check=True,capture_output=True)
            subprocess.run([*prefix,'shell','chmod','755',remote],check=True,capture_output=True)
            run=subprocess.run([*prefix,'shell',remote],check=True,capture_output=True,text=True)
            assert 'PASS' in run.stdout;result.update(device=args.device,stdout=run.stdout,status='PASS')
            subprocess.run([*prefix,'shell','rm',remote],check=True,capture_output=True)
        results.append(result)
    receipt={'hook_sha256':hashlib.sha256((ROOT/'tools/display_hooks.py').read_bytes()).hexdigest(),'editions':results}
    (ROOT/'generated/display-opaque-runs.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))
