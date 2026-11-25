"""Compare native neighbor expansion against the lifted Crusader route search."""
import argparse
import re
import subprocess
from pathlib import Path

import build_android
import route_hooks
from test_display_rle import HEADER

ROOT = Path(__file__).resolve().parents[1]
LAYOUT = {
    "classic": (0x2344480, 0x24CCAE8, 0x1BD0E98, 0x4724F0),
    "game": (0x2DD7960, 0x2F5FFC8, 0x2664378, 0x472700),
}

CHECK = r"""
#define ROUTE 0x12000000u
static uint8_t queue[0xB8C00], visit[80400*2], depth[80400*2], stack[128];
static uint32_t yields[32][10];
static unsigned count_yields, native_groups;
static const int dx[]={1,1,0,-1,-1,-1,0,1}, dy[]={0,1,1,1,0,-1,-1,-1};

static void registers(uint32_t *result)
{
    uint32_t values[]={g_eax,g_ebx,g_ecx,g_edx,g_esi,g_edi,g_esp,g_ebp,
                       g_flag_a,g_flag_b,g_flag_cf,g_flag_k,g_backedges,g_cur_func};
    memcpy(result,values,sizeof values);
}

static void record_yield(void)
{
    assert(count_yields<32);
    uint32_t values[]={g_eax,g_ebx,g_ecx,g_edx,g_esi,g_edi,g_esp,g_ebp,
                       MEM32(ROUTE+0x1BB18),MEM32(ROUTE+0x1BB20)};
    memcpy(yields[count_yields++],values,sizeof values);
}

static void reset(unsigned n,uint32_t budget)
{
    memset(PTR(ROUTE),0,sizeof queue);
    memset(PTR(VISITED),0,sizeof visit);
    memset(PTR(DEPTH),0,sizeof depth);
    memset(PTR(0x15007F80),0xA7,sizeof stack);
    for(unsigned d=0;d<8;d++){
        MEM8(DIRECTION+d)=1u<<d;
        MEM16(DELTA+8*d)=(uint16_t)dx[d];
        MEM16(DELTA+8*d+4)=(uint16_t)dy[d];
    }
    for(int y=0;y<44;y++){
        MEM32(ROW+12*y)=64*(y+2);
        for(unsigned d=0;d<8;d++) MEM32(NEIGHBORS+32*y+4*d)=dx[d]+64*dy[d];
        for(int x=0;x<44;x++){
            uint32_t tile=64*(y+2)+x;
            int inner=x>4&&x<39&&y>4&&y<39;
            MEM8(VALID+400*y+x)=inner;
            MEM32(FLAGS+4*tile)=inner ? (uint32_t)(((x+y+n)%5==0)*0x100u) : 0x4A5014B1;
            MEM16(HEIGHT+2*tile)=inner ? (x*3+y+n)%31==0 : 1;
            uint8_t bits=0;
            if(x>5&&x<38&&y>5&&y<38)
                for(unsigned d=0;d<8;d++) if(!n||(x*17+y*13+d*7+n)%11<7)bits|=1u<<d;
            MEM8(LINKS+tile)=bits;
        }
    }
    uint32_t args[]={0xDEADC0DE,6+n%27,7+n%29,n%11?28+n%7:UINT32_MAX,28+n%8,n%7?6000:16,0};
    if (n % 13 == 0) {
        unsigned head = 80398;
        uint32_t tile = MEM32(ROW + 12 * args[2]) + args[1];
        args[5] = 100000;
        args[6] = 1;
        MEM32(ROUTE + 4) = 12;
        MEM32(ROUTE + 0x1BB14) = 1;
        MEM32(ROUTE + 0x1BB18) = head;
        MEM32(ROUTE + 0x1BB20) = head + 1;
        MEM32(ROUTE + 0x1BB40 + 4 * head) = tile;
        MEM16(ROUTE + 0x6A380 + 2 * head) = args[2];
        MEM16(ROUTE + 0x917A0 + 2 * head) = args[1];
    }
    g_esp=0x15008000;memcpy(PTR(g_esp),args,sizeof args);
    g_eax=0x1006;g_ebx=0x1009;g_ecx=ROUTE;g_edx=0x1010;
    g_esi=0x1011;g_edi=0x1014;g_ebp=0x1016;
    g_flag_a=g_flag_b=g_flag_cf=g_flag_k=0;
    g_backedges=budget;
    count_yields=0;
    recomp_yield_hook=budget?record_yield:NULL;
}

int main(void)
{
    assert(mmap((void*)VOID_GUEST_BASE,VOID_GUEST_SPAN,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_NORESERVE,-1,0)==(void*)VOID_GUEST_BASE);
    for(unsigned n=0;n<120;n++)for(unsigned budget=0;budget<2;budget++){
        uint32_t before[14],after[14], archived_yields[32][10];
        uint32_t start=budget?RECOMP_YIELD_EVERY-2-n%17:0;
        reset(n,start); route_original(); registers(before);
        memcpy(queue,PTR(ROUTE),sizeof queue);
        memcpy(visit,PTR(VISITED),sizeof visit);
        memcpy(depth,PTR(DEPTH),sizeof depth);
        memcpy(stack,PTR(0x15007F80),sizeof stack);
        unsigned checks=count_yields;
        memcpy(archived_yields,yields,checks*sizeof *yields);
        reset(n,start); route_native(); registers(after);
        assert(!memcmp(before,after,sizeof before));
        assert(count_yields==checks&&!memcmp(archived_yields,yields,checks*sizeof *yields));
        assert(!memcmp(queue,PTR(ROUTE),sizeof queue));
        assert(!memcmp(visit,PTR(VISITED),sizeof visit));
        assert(!memcmp(depth,PTR(DEPTH),sizeof depth));
        assert(!memcmp(stack,PTR(0x15007F80),sizeof stack));
    }
    assert(native_groups>1000);
    printf("240 route cases, %u native groups, queues, visits, distances, registers and yields PASS\n",native_groups);
}
"""


def build(module):
    start, visited, depth, links, flags, height, direction, delta = route_hooks.LAYOUT[module]
    valid, row, neighbors, clear = LAYOUT[module]
    target = ROOT / "build/native-route-tests"
    target.mkdir(parents=True, exist_ok=True)
    function = rb"(?ms)^void sub_%08X\(void\) \{.*?^\}" % start
    source = next(data[0] for p in (ROOT / f"generated/{module}-x86").glob("recomp_*.c")
                  if (data := re.search(function, p.read_bytes())))
    native = route_hooks.apply(source, module)
    assert native != source
    names = "VISITED DEPTH LINKS FLAGS HEIGHT DIRECTION DELTA VALID ROW NEIGHBORS".split()
    defines = "".join(f"#define {name} 0x{addr:X}u\n" for name, addr in zip(
        names, (visited, depth, links, flags, height, direction, delta, valid, row, neighbors)))
    prelude = f"static unsigned native_groups;\nvoid sub_{clear:08X}(void) {{ abort(); }}\n".encode()
    native = native.replace(b"for (unsigned _i = 0;", b"++native_groups; for (unsigned _i = 0;", 1)
    code = (HEADER.encode() + defines.encode() + prelude + source.replace(b"sub_%08X" % start, b"route_original", 1) +
            b"\n" + native.replace(b"sub_%08X" % start, b"route_native", 1) + CHECK.encode())
    path = target / (module + ".c")
    path.write_bytes(code)
    binary = target / module
    subprocess.run([str(build_android.CLANG), f"--target={build_android.TARGET}", "-O2", "-fwrapv", "-fno-strict-aliasing",
                    "-Iandroid/runtime", str(path), "-o", str(binary)], cwd=ROOT, check=True)
    return binary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device")
    args = parser.parse_args()
    for module in LAYOUT:
        binary = build(module)
        if args.device:
            adb = ["adb", "-s", args.device]
            dest = f"/data/local/tmp/stronghold-route-test-{module}"
            subprocess.run([*adb, "push", str(binary), dest], check=True)
            subprocess.run([*adb, "shell", "chmod", "755", dest], check=True)
            subprocess.run([*adb, "shell", dest], check=True)
