"""Prove that the exact skipped TGX capture sites write only the original x86 stack."""
import json
from pathlib import Path
import re
import struct
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import capstone
from capstone.x86 import X86_OP_MEM,X86_REG_EBP
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_EBP,UC_X86_REG_EIP,UC_X86_REG_ESP
import display_hooks
import original_hooks
from test_game_text import Editor
from test_display_rle import LAYOUT


def proof(module):
    original=Editor('extreme' if module=='game' else module)
    entry,renderer,screen=LAYOUT[module]
    md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);md.detail=True
    instructions=list(md.disasm(bytes(original.u.mem_read(entry,0x1136)),entry))
    assert sum(i.size for i in instructions)==0x1136
    ebp_writers={i.address-entry for i in instructions if X86_REG_EBP in i.regs_access()[1]}
    assert ebp_writers=={1,0x1132},ebp_writers
    stores={i.address:i.operands[0].mem.disp for i in instructions if i.operands and
            i.operands[0].type==X86_OP_MEM and i.operands[0].access&capstone.CS_AC_WRITE and
            i.operands[0].mem.base==X86_REG_EBP}
    assert set(stores)=={entry+offset for offset in display_hooks.RLE_STACK_STORES}
    assert all(i.operands[0].size==4 for i in instructions if i.address in stores)
    pattern=rb'(?ms)^void sub_%08X\(void\) \{.*?^\}'%entry
    body=next(m[0] for p in (ROOT/f'generated/{module}-x86').glob('recomp_*.c')
              if (m:=re.search(pattern,p.read_bytes())))
    patched=display_hooks.apply(b'#include "recomp_funcs.h"\n'+body,module)[0]
    if module=='stronghold':patched=original_hooks.apply(patched,module)[0]
    for pc in stores:
        line=next(line for line in patched.splitlines() if b'/* 0x%08X:'%pc in line)
        assert display_hooks.raster_stack_store(line,pc,module)
        assert b'stronghold_display_' not in line,line
    # Only the known stack slots are excluded; actual pixel stores still capture.
    for start,_,_ in display_hooks.HALF_RUNS[module]:
        pc=start+(8 if start==display_hooks.HALF_RUNS[module][0][0] else 11)
        line=next(line for line in patched.splitlines() if b'/* 0x%08X:'%pc in line)
        assert b'stronghold_display_written(edi + ebx,2)' in line or b'stronghold_display_written(edi + ebx, 2)' in line,line
    assert not display_hooks.raster_stack_store(b'    MEM32(edi) = eax;',entry+9,module)
    assert not display_hooks.raster_stack_store(b'    MEM32(ebp + (-0x20)) = eax;',entry+10,module)

    u=original.u;dst,src=0x12000000,0x14000000
    u.mem_map(dst,0x10000);u.mem_map(src,0x1000)
    observed={};pixels=[0]
    def write(uc,access,address,size,value,user):
        pc=uc.reg_read(UC_X86_REG_EIP)
        if pc in stores:
            assert size==4 and address==uc.reg_read(UC_X86_REG_EBP)+stores[pc]
            assert 0x10000000<=address and address+size<0x10020000
            observed[pc]=observed.get(pc,0)+1
        elif dst<=address<dst+0x10000:pixels[0]+=1
    token=u.hook_add(UC_HOOK_MEM_WRITE,write)
    cases=0
    try:
        for n in (1,2,8,31,32):
            for fill in (False,True):
                for odd in (False,True):
                    data=bytearray()
                    for row in range(4):
                        if odd:data.append(0x20)
                        data.append((0x40 if fill else 0)|(n-1))
                        for x in range(1 if fill else n):data.extend(struct.pack('<H',0x1234+x*97+row*319))
                        if n<32:data.append(0x20|(31-n))
                        data.append(0x80)
                    u.mem_write(src,bytes(data));u.mem_write(dst,b'\xa5'*0x10000)
                    original.w(screen+0xD8,dst);original.w(renderer+0x54,1)
                    original.w(renderer+0x58,0);original.w(renderer+0x5C,4)
                    u.reg_write(UC_X86_REG_EBP,0xAABBCCDD)
                    original.run(entry,(0,0,32,4,src),this=renderer)
                    assert u.reg_read(UC_X86_REG_EIP)==original.end
                    assert u.reg_read(UC_X86_REG_ESP)==original.stack+24
                    assert u.reg_read(UC_X86_REG_EBP)==0xAABBCCDD
                    cases+=1
    finally:u.hook_del(token)
    assert pixels[0]>0 and entry+0x10D8 in observed and entry+0x110B in observed
    return dict(sha256=original.sha,proven_store_sites=len(stores),original_x86_cases=cases,
                observed_stack_writes=sum(observed.values()),observed_sites=len(observed),pixel_writes=pixels[0])


if __name__=='__main__':
    results={module:proof(module) for module in LAYOUT}
    (ROOT/'generated/raster-stack-proof.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2))
