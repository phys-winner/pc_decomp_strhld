"""Compare the lifted and batched half-scale indexed sprite commands on ARM64."""
import argparse
import re
import subprocess

import build_android
import display_hooks
from test_display_rle import HEADER, ROOT


DRIVER = r'''
static struct {
    uint32_t regs[13], yields[8][8];
    unsigned yield_count;
    uint8_t pixels[256];
} result, reference;

static void record_yield(void)
{
    uint32_t regs[] = {g_eax,g_ecx,g_edx,g_ebx,g_esp,g_ebp,g_esi,g_edi};
    assert(result.yield_count < 8);
    memcpy(result.yields[result.yield_count++],regs,sizeof regs);
}

static void run(void (*draw)(void), unsigned kind, unsigned length, unsigned parity,
                unsigned seed, unsigned stop, int capture)
{
    g_eax=0xABCD1234;g_ecx=0x5678;g_edx=0x12000101;g_ebx=0x4321;
    g_edi=0x12000400;g_esi=0x12000600;g_esp=0x12000900;g_ebp=g_esp+64;
    MEM32(g_ebp-0x20u)=parity;
    g_flag_a=g_flag_b=g_flag_k=g_flag_cf=0;g_backedges=seed;
    recomp_yield_hook=seed==0xFFFFFFF0u?NULL:record_yield;
    stronghold_display_capture_active=capture;
    memset(&result,0,sizeof result);
    memset(PTR(g_edi),0xA5,sizeof result.pixels);
    for(unsigned i=0;i<256;i++)MEM16(g_edx+i*2)=(uint16_t)(0xA1B2u+i*15917u);
    uint8_t *source=PTR(g_esi);
    for(unsigned i=0;i<4;i++){
        unsigned count=1+(length-1+i*13)%32;
        *source++=(uint8_t)((kind<<5)|(count-1));
        for(unsigned j=0;j<(kind==0?count:kind==2);j++)*source++=(uint8_t)(i*97+j*23+length*11);
        kind=(kind+1)%3;
    }
    *source=(uint8_t)stop;
    draw();
    uint32_t regs[]={g_eax,g_ecx,g_edx,g_ebx,g_esp,g_ebp,g_esi,g_edi,g_flag_a,g_flag_b,
                     g_flag_k,g_flag_cf,MEM32(g_ebp-0x20u)};
    memcpy(result.regs,regs,sizeof regs);
    memcpy(result.pixels,PTR(g_edi),sizeof result.pixels);
}

int main(void)
{
    assert(mmap((void*)VOID_GUEST_BASE,VOID_GUEST_SPAN,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_NORESERVE,-1,0)==(void*)VOID_GUEST_BASE);
    unsigned cases=0, commands=0, sched=0;
    unsigned stops[]={0x60,0x80,0xFF};
    for(unsigned kind=0;kind<3;kind++)for(unsigned length=1;length<=32;length++)
    for(unsigned parity=0;parity<2;parity++)for(unsigned i=0;i<38;i++)
    for(unsigned stop=0;stop<3;stop++)for(int capture=0;capture<2;capture++){
        unsigned seed=i==36?0:i==37?0xFFFFFFF0u:RECOMP_YIELD_EVERY-36+i;
        run(lifted,kind,length,parity,seed,stops[stop],capture);
        unsigned edges=g_backedges;
        memcpy(&reference,&result,sizeof reference);
        fast_commands=0;
        run(native,kind,length,parity,seed,stops[stop],capture);
        assert(!memcmp(&reference,&result,sizeof reference)&&edges==g_backedges);
        if(capture)assert(fast_commands==0);
        if(!capture&&(i==36||i==37))assert(fast_commands==4);
        commands+=fast_commands;
        sched+=result.yield_count;
        cases++;
    }
    printf("Half-scale palette ARM64: %u cases, %u batched commands, %u exact yields PASS\n",
           cases,commands,sched);
}
'''


def fragment(body, start, name, native=False):
    label = lambda delta: b'L_%08X:\n' % (start + delta)
    dispatch = body[body.index(label(0)):body.index(label(0x21))]
    commands = body[body.index(label(0x6E)):body.index(label(0xDF))]
    if native:
        dispatch = dispatch.replace(b'g_backedges += _edges;', b'++fast_commands; g_backedges += _edges;')
    return (b'\nvoid %s(void) {\n    uint32_t _flag_a=0,_flag_b=0,_flag_k=0,_cf=0;\n'
            b'    RECOMP_REGS_LOCALS\n' % name + dispatch + label(0x21) + b'    goto done;\n' +
            commands + b'done:\n    RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT();\n}\n')


def build(module, out):
    start, functions = display_hooks.HALF_PALETTE[module]
    pattern = rb'(?ms)^void sub_%08X\(void\) \{.*?^\}' % functions[0]
    source = next(m[0] for p in (ROOT / f'generated/{module}-x86').glob('recomp_*.c')
                  if (m := re.search(pattern, p.read_bytes())))
    patched, count = display_hooks.native_half_commands(source, module, palette=True)
    assert count == 1
    raw, patched = (body.replace(b'\r\n', b'\n') for body in (source, patched))
    declarations = b'\nint stronghold_display_capture_active; static unsigned fast_commands;\n'
    code = out / f'{module}.c'
    code.write_bytes(HEADER.encode() + declarations + fragment(raw, start, b'lifted') +
                     fragment(patched, start, b'native', True) + DRIVER.encode())
    binary = out / module
    subprocess.run([build_android.CLANG, f'--target=aarch64-linux-android{build_android.API}',
                    '-O2', '-std=gnu11', '-fno-strict-aliasing', '-fwrapv',
                    f'-I{ROOT / "android/runtime"}', code, '-Wl,-z,max-page-size=16384',
                    '-lm', '-o', binary], check=True)
    return binary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    args = parser.parse_args()
    out = ROOT / 'build/display-palette-runs'
    out.mkdir(parents=True, exist_ok=True)
    adb = ['adb', '-s', args.device]
    for module in display_hooks.HALF_PALETTE:
        binary = build(module, out)
        remote = '/data/local/tmp/stronghold-palette-' + module
        try:
            subprocess.run([*adb, 'push', binary, remote], check=True)
            subprocess.run([*adb, 'shell', 'chmod', '755', remote], check=True)
            subprocess.run([*adb, 'shell', remote], check=True)
        finally:
            subprocess.run([*adb, 'shell', 'rm', '-f', remote], check=True)
