"""Build actual lifted ARM64 half-size RLE bodies before/after bounded batching.

The executable is a CLI-only fixture. It compares pixels, callee state, every
raw backedge and yield count, and the maximum yield displacement in pixels.
"""
import hashlib
import argparse
import json
import re
import subprocess
from pathlib import Path

import build_android
import display_hooks

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/display-rle'

HEADER = r'''
#define RECOMP_GENERATED_CODE
#define RECOMP_LOCAL_REGS
#define RECOMP_PLAIN_MEMORY
#include "recomp_types.h"
#include <assert.h>
#include <sys/mman.h>
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(target) abort()
#define PTR(a) ((void *)ADDR(a))
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_cur_func;
uint32_t g_flag_k,g_flag_a,g_flag_b,g_flag_cf,g_backedges;
void (*recomp_yield_hook)(void);
static unsigned pixel, yield_count, positions[32];
static void yield_record(void){assert(yield_count<32);positions[yield_count++]=pixel;}
'''

FOOTER = r'''
static uint32_t state[12], expected[12];
static void snapshot(uint32_t *out){
 uint32_t a[]={g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,
               g_flag_k,g_flag_a,g_flag_b,g_flag_cf};memcpy(out,a,sizeof(a));
}
static void setup(unsigned n,unsigned kind,unsigned seed,int yielding){
 memset(PTR(0x12000000),0xA5,8112*4+128);
 uint8_t *p=PTR(0x14000000);
 for(unsigned y=0;y<4;y++){
  *p++=(kind?0x40:0)|(n-1);
  for(unsigned x=0;x<(kind?1:n);x++){
   uint16_t c=(uint16_t)(0x1234+x*97+y*319);memcpy(p,&c,2);p+=2;
  }
  if(n<32)*p++=0x20|(31-n);
  *p++=0x80;
 }
 MEM32(SCREEN+0xD8)=0x12000000;
 MEM32(RENDERER+0x54)=1;MEM32(RENDERER+0x58)=0;MEM32(RENDERER+0x5C)=4;
 g_eax=0x11223344;g_ecx=RENDERER;g_edx=0x55667788;g_ebx=0x1234;
 g_esi=0x5678;g_edi=0x9ABC;g_ebp=0xDEAD;g_esp=0x15008000;
 uint32_t args[]={0xDEADC0DE,0,0,32,4,0x14000000};memcpy(PTR(g_esp),args,sizeof(args));
 g_flag_k=g_flag_a=g_flag_b=g_flag_cf=0;
 g_backedges=seed;pixel=yield_count=0;memset(positions,0,sizeof(positions));
 recomp_yield_hook=yielding?yield_record:NULL;
}
int main(void){
 void *m=mmap((void*)VOID_GUEST_BASE,VOID_GUEST_SPAN,PROT_READ|PROT_WRITE,
             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_NORESERVE,-1,0);
 assert(m==(void*)VOID_GUEST_BASE);
 uint8_t *reference=malloc(8112*4+128);assert(reference);
 unsigned cases=0,maximum_delay=0,observed_pixels=0;
 for(unsigned kind=0;kind<2;kind++)for(unsigned n=1;n<=32;n++)
 for(unsigned s=0;s<38;s++){
  unsigned seed=s==36?0:s==37?0xFFFFFFF0u:65500+s;
  int yielding=s!=37;
  setup(n,kind,seed,yielding);rle_original();snapshot(expected);
  memcpy(reference,PTR(0x12000000),8112*4+128);
  unsigned counter=g_backedges,yields=yield_count,old_positions[32];
  memcpy(old_positions,positions,sizeof(positions));
  assert(pixel==n*2);observed_pixels+=pixel;
  setup(n,kind,seed,yielding);rle_batched();snapshot(state);
  assert(!memcmp(reference,PTR(0x12000000),8112*4+128));
  assert(!memcmp(expected,state,sizeof(state)));
  assert(g_backedges==counter&&yield_count==yields&&pixel==n*2);
  for(unsigned i=0;i<yields;i++){
   assert(positions[i]>=old_positions[i]);
   unsigned delay=positions[i]-old_positions[i];assert(delay<=31);
   if(delay>maximum_delay)maximum_delay=delay;
  }
  cases++;
 }
 assert(maximum_delay==31);
 printf("RLE actual ARM64: %u cases, %u pixel iterations, exact pixels/registers/raw-budget/yields PASS; maximum delayed pixels %u\n",cases,observed_pixels,maximum_delay);
 return 0;
}
'''


def build(module):
    address = 0x44D5D0 if module == 'classic' else 0x44D810
    pattern = rb'(?ms)^void sub_%08X\(void\) \{.*?^\}' % address
    original = next(m[0] for p in (ROOT / f'generated/{module}-x86').glob('recomp_*.c')
                    if (m := re.search(pattern, p.read_bytes())))
    batched, count = display_hooks.batch_rle_backedges(original, module)
    assert count == 2
    # Instrument only the two executed pixel-loop boundaries, outside production.
    for pc in display_hooks.RLE_BACKEDGES[module]:
        pat = (rb'(    )(RECOMP_BACKEDGE\(\);|\+\+_display_rle_budget;)\r?\n'
               rb'(    if \([^\r\n]+/\* 0x%08X:[^\r\n]+)' % pc)
        original, n = re.subn(pat, rb'\1++pixel; \2\n\3', original)
        assert n == 1
        batched, n = re.subn(pat, rb'\1++pixel; \2\n\3', batched)
        assert n == 1
    original = original.replace(b'sub_%08X' % address, b'rle_original', 1)
    batched = batched.replace(b'sub_%08X' % address, b'rle_batched', 1)
    renderer, screen = (0x21288D8, 0xFD5780) if module == 'classic' else (0x2BBBDB8, 0xFD5BE0)
    defines = f'\n#define RENDERER 0x{renderer:X}u\n#define SCREEN 0x{screen:X}u\n'
    source = OUT / f'{module}.c'
    source.write_bytes(HEADER.encode() + defines.encode() + original + b'\n' + batched + FOOTER.encode())
    binary = OUT / module
    subprocess.run([str(build_android.CLANG), '--target=aarch64-linux-android30', '-O2', '-std=gnu11',
                    '-fno-strict-aliasing', '-fwrapv', f'-I{ROOT / "android/runtime"}', str(source),
                    '-Wl,-z,max-page-size=16384', '-lm', '-o', str(binary)], check=True)
    return dict(edition=module, source=str(source.relative_to(ROOT)),
                source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                binary=str(binary.relative_to(ROOT)), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                cases=2432, maximum_delay_pixels=31)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', help='Optional adb serial; runs only isolated CLI fixtures.')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    result = dict(status='BUILT', hook_sha256=hashlib.sha256((ROOT / 'tools/display_hooks.py').read_bytes()).hexdigest(),
                  editions=[build(m) for m in ('classic', 'game')])
    if args.device:
        adb = Path.home() / 'AppData/Local/Android/Sdk/platform-tools/adb.exe'
        for edition in result['editions']:
            destination = '/data/local/tmp/stronghold-rle-' + edition['edition']
            prefix = [str(adb), '-s', args.device]
            subprocess.run([*prefix, 'push', str(ROOT / edition['binary']), destination], check=True, capture_output=True)
            subprocess.run([*prefix, 'shell', 'chmod', '755', destination], check=True, capture_output=True)
            run = subprocess.run([*prefix, 'shell', destination], check=True, capture_output=True, text=True)
            assert 'maximum delayed pixels 31' in run.stdout
            edition.update(device=args.device, exit_code=run.returncode, stdout=run.stdout.strip(), status='PASS')
        result['status'] = 'PASS'
    (ROOT / 'generated/display-rle-batching.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Actual ARM64 RLE fixtures: ' + result['status'] + ' for both editions.')
